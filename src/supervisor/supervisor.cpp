#include "supervisor/supervisor.hpp"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <iomanip>
#include <optional>
#include <sstream>

#include "config/config_parser.hpp"
#include "dependency/dependency_graph.hpp"
#include "linux/signal_pipe.hpp"
#include "linux/sysfs_reader.hpp"
#include "logging/logger.hpp"

namespace pilot {

namespace {

constexpr const char* kComponent = "Supervisor";

Logger& logger() { return Logger::instance(); }

std::string formatUptime(std::chrono::seconds s) {
    const long long total = s.count();
    std::ostringstream out;
    out << total / 3600 << "h" << std::setw(2) << std::setfill('0') << (total % 3600) / 60 << "m"
        << std::setw(2) << std::setfill('0') << total % 60 << "s";
    return out.str();
}

}  // namespace

ProcessSupervisor::ProcessSupervisor(SupervisorOptions options) : options_(std::move(options)) {}

ProcessSupervisor::~ProcessSupervisor() {
    // Make sure helper threads are gone before the members they reference are destroyed.
    if (monitor_) monitor_->stop();
    if (control_) control_->stop();
}

Result<void> ProcessSupervisor::loadConfiguration(const std::filesystem::path& path) {
    auto defs = ConfigParser{}.parseFile(path);
    if (!defs) return defs.error();
    return loadDefinitions(std::move(defs.value()));
}

Result<void> ProcessSupervisor::loadDefinitions(std::vector<ServiceDefinition> definitions) {
    std::scoped_lock lock(mutex_);
    if (configured_) {
        return makeError(ErrorCode::InvalidState, "configuration already loaded");
    }
    if (auto r = validateServiceSet(definitions); !r) return r.error();

    DependencyGraph graph;
    for (const auto& d : definitions) {
        if (auto r = validateServiceDefinition(d); !r) return r.error();
        if (auto r = graph.addService(d); !r) return r.error();
    }
    auto order = graph.topologicalOrder();  // detects missing dependencies and cycles
    if (!order) return order.error();

    std::unordered_map<std::string, ServiceDefinition> services;
    for (auto& d : definitions) {
        services.emplace(d.name, std::move(d));
    }

    std::vector<std::string> startOrder;
    for (const auto& name : order.value()) {
        if (services.at(name).enabled) startOrder.push_back(name);
    }

    for (const auto& name : startOrder) {
        if (auto r = registry_.registerService(name); !r) return r.error();
        policies_.emplace(name, RestartPolicy(services.at(name).restart));
    }

    services_ = std::move(services);
    startOrder_ = std::move(startOrder);
    stopOrder_.assign(startOrder_.rbegin(), startOrder_.rend());

    monitor_ = std::make_unique<HealthMonitor>(registry_, ProcReader{},
                                               [this](const HealthIssue& issue) { onHealthIssue(issue); });
    std::unordered_map<std::string, HealthSettings> settings;
    for (const auto& name : startOrder_) {
        const auto& h = services_.at(name).health;
        if (h.mode == HealthCheckMode::Process) {
            settings.emplace(name, HealthSettings{h.interval, h.maxRssKb});
        }
    }
    monitor_->configure(std::move(settings));

    configured_ = true;
    logger().info(kComponent, "Loaded " + std::to_string(startOrder_.size()) + " enabled service(s)");
    return Result<void>::success();
}

Result<void> ProcessSupervisor::installSignalHandlers() { return SignalPipe::install(); }

void ProcessSupervisor::requestShutdown() noexcept {
    shutdownRequested_.store(true);
    SignalPipe::notify(0);
}

// ---------------------------------------------------------------- lifecycle (mutex_ held)

void ProcessSupervisor::forceState(const std::string& name, ProcessState state) {
    const auto info = registry_.get(name);
    if (!info || info->state == state) return;
    if (auto r = registry_.transition(name, state); !r) {
        logger().error(kComponent, r.error().message);
    }
}

Result<void> ProcessSupervisor::checkDependenciesRunning(const std::string& name) const {
    for (const auto& dep : services_.at(name).dependencies) {
        const auto info = registry_.get(dep);
        if (!info || info->state != ProcessState::Running) {
            return makeError(ErrorCode::InvalidState,
                             "dependency '" + dep + "' of '" + name + "' is not running");
        }
    }
    return Result<void>::success();
}

Result<void> ProcessSupervisor::startService(const std::string& name) {
    const auto info = registry_.get(name);
    if (!info) return makeError(ErrorCode::ProcessNotFound, "unknown service '" + name + "'");
    if (info->state == ProcessState::Running) return Result<void>::success();

    if (auto r = registry_.transition(name, ProcessState::Starting); !r) return r;

    logger().info(kComponent, "Starting " + name);
    auto pid = manager_.start(services_.at(name));
    if (!pid) {
        forceState(name, ProcessState::Failed);
        logger().error(name, pid.error().describe());
        return pid.error();
    }
    if (auto r = registry_.markRunning(name, pid.value(), std::chrono::steady_clock::now()); !r) {
        (void)manager_.stop(pid.value(), std::chrono::milliseconds(1000));
        forceState(name, ProcessState::Failed);
        return r;
    }
    logger().info(name, "Started with PID " + std::to_string(pid.value()), pid.value());
    return Result<void>::success();
}

Result<void> ProcessSupervisor::stopService(const std::string& name) {
    const auto info = registry_.get(name);
    if (!info) return makeError(ErrorCode::ProcessNotFound, "unknown service '" + name + "'");
    pendingRestarts_.erase(name);

    switch (info->state) {
        case ProcessState::Running: {
            if (auto r = registry_.transition(name, ProcessState::Stopping); !r) return r;
            logger().info(kComponent, "Stopping " + name, info->pid);
            auto status = manager_.stop(info->pid, services_.at(name).stopTimeout);
            registry_.clearPid(name);
            forceState(name, ProcessState::Stopped);
            if (!status) {
                logger().error(name, "stop failed: " + status.error().describe(), info->pid);
                return status.error();
            }
            logger().info(name, "Stopped (" + status.value().describe() + ")", info->pid);
            return Result<void>::success();
        }
        case ProcessState::Restarting:
        case ProcessState::Failed:
        case ProcessState::PermanentlyFailed:
            forceState(name, ProcessState::Stopped);
            return Result<void>::success();
        case ProcessState::Stopped:
        case ProcessState::Starting:
        case ProcessState::Stopping:
            return Result<void>::success();
    }
    return Result<void>::success();
}

Result<void> ProcessSupervisor::startAll() {
    std::scoped_lock lock(mutex_);
    if (!configured_) return makeError(ErrorCode::InvalidState, "no configuration loaded");

    std::vector<std::string> startedNow;
    for (const auto& name : startOrder_) {
        const auto info = registry_.get(name);
        if (info && info->state == ProcessState::Running) continue;

        Result<void> r = checkDependenciesRunning(name);
        if (r) r = startService(name);
        if (!r) {
            logger().error(kComponent, "Startup of '" + name + "' failed, rolling back: " + r.error().message);
            for (auto it = startedNow.rbegin(); it != startedNow.rend(); ++it) {
                if (auto s = stopService(*it); !s) logger().error(kComponent, s.error().message);
            }
            return r;
        }
        startedNow.push_back(name);
    }
    logger().info(kComponent, "All services started");
    return Result<void>::success();
}

Result<void> ProcessSupervisor::stopAll() {
    std::scoped_lock lock(mutex_);
    std::optional<Error> firstError;
    for (const auto& name : stopOrder_) {
        if (auto r = stopService(name); !r && !firstError) firstError = r.error();
    }
    if (firstError) return *firstError;
    return Result<void>::success();
}

Result<void> ProcessSupervisor::restartService(const std::string& name) {
    if (shutdownRequested_.load()) {
        return makeError(ErrorCode::InvalidState, "supervisor is shutting down");
    }
    std::scoped_lock lock(mutex_);
    const auto it = services_.find(name);
    if (it == services_.end() || !registry_.contains(name)) {
        return makeError(ErrorCode::ProcessNotFound, "unknown or disabled service '" + name + "'");
    }
    logger().info(kComponent, "Manual restart of " + name);
    if (auto r = stopService(name); !r) return r;
    policies_.at(name).reset();
    if (auto r = checkDependenciesRunning(name); !r) return r;
    if (auto r = startService(name); !r) return r;
    if (auto r = registry_.incrementRestartCount(name); !r) return r;
    return Result<void>::success();
}

Result<void> ProcessSupervisor::restartAll() {
    if (shutdownRequested_.load()) {
        return makeError(ErrorCode::InvalidState, "supervisor is shutting down");
    }
    if (auto r = stopAll(); !r) return r;
    for (const auto& name : startOrder_) {
        std::scoped_lock lock(mutex_);
        policies_.at(name).reset();
    }
    return startAll();
}

// ---------------------------------------------------------------- exits & restarts (mutex_ held)

void ProcessSupervisor::reapChildren() {
    for (const ProcessExitStatus& st : manager_.reapAll()) {
        handleExit(st);
    }
}

void ProcessSupervisor::handleExit(const ProcessExitStatus& st) {
    const auto name = registry_.findByPid(st.pid);
    if (!name) {
        logger().debug(kComponent, "Reaped unknown child: " + st.describe(), st.pid);
        return;
    }
    const auto info = registry_.get(*name);
    if (!info || info->state != ProcessState::Running) {
        return;  // intentional stop already accounted for
    }
    registry_.clearPid(*name);
    const bool abnormal = st.abnormal();
    logger().log(abnormal ? LogLevel::Error : LogLevel::Info, *name,
              "Process exited " + std::string(abnormal ? "unexpectedly: " : "cleanly: ") + st.describe(), st.pid);
    applyExitPolicy(*name, abnormal, st.describe());
}

void ProcessSupervisor::applyExitPolicy(const std::string& name, bool abnormal, const std::string& what) {
    const auto now = std::chrono::steady_clock::now();
    const RestartDecision d = policies_.at(name).recordExit(abnormal, now);

    switch (d.action) {
        case RestartAction::DoNotRestart:
            forceState(name, abnormal ? ProcessState::Failed : ProcessState::Stopped);
            logger().info(kComponent, "Not restarting " + name + ": " + d.reason);
            break;
        case RestartAction::Restart:
            forceState(name, ProcessState::Failed);
            forceState(name, ProcessState::Restarting);
            pendingRestarts_[name] = now + d.delay;
            logger().warning(kComponent, "Restarting " + name + " in " + std::to_string(d.delay.count()) +
                                          " ms (" + d.reason + "; last exit: " + what + ")");
            break;
        case RestartAction::GiveUp:
            forceState(name, ProcessState::Failed);
            forceState(name, ProcessState::PermanentlyFailed);
            pendingRestarts_.erase(name);
            logger().critical(kComponent, name + " is PERMANENTLY_FAILED: " + d.reason);
            break;
    }
}

void ProcessSupervisor::processDueRestarts() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> due;
    for (const auto& [name, when] : pendingRestarts_) {
        if (when <= now) due.push_back(name);
    }
    std::sort(due.begin(), due.end(), [this](const std::string& a, const std::string& b) {
        const auto pa = std::find(startOrder_.begin(), startOrder_.end(), a);
        const auto pb = std::find(startOrder_.begin(), startOrder_.end(), b);
        return pa < pb;  // dependencies first
    });

    for (const auto& name : due) {
        pendingRestarts_.erase(name);
        const auto info = registry_.get(name);
        if (!info || info->state != ProcessState::Restarting) continue;

        if (auto r = startService(name); !r) {
            // The failed start (state is FAILED now) counts as another failure.
            applyExitPolicy(name, true, "start failed: " + r.error().message);
            continue;
        }
        if (auto r = registry_.incrementRestartCount(name); !r) logger().error(kComponent, r.error().message);
        const auto fresh = registry_.get(name);
        logger().info(kComponent, "Restarted " + name + " (restart #" +
                                   std::to_string(fresh ? fresh->restartCount : 0) + ")",
                   fresh ? fresh->pid : 0);
    }
}

void ProcessSupervisor::onHealthIssue(const HealthIssue& issue) {
    if (shutdownRequested_.load()) return;
    std::scoped_lock lock(mutex_);

    reapChildren();  // a plain crash is handled by the normal exit path first
    const auto info = registry_.get(issue.service);
    if (!info || info->state != ProcessState::Running || info->pid != issue.pid) return;

    logger().warning(issue.service,
                  "Unhealthy (" + std::string(toString(issue.kind)) + "): " + issue.detail + " - terminating",
                  issue.pid);
    auto status = manager_.stop(issue.pid, services_.at(issue.service).stopTimeout);
    registry_.clearPid(issue.service);
    applyExitPolicy(issue.service, true,
                    std::string(toString(issue.kind)) + (status ? "; " + status.value().describe() : ""));
}

// ---------------------------------------------------------------- main loop

std::chrono::milliseconds ProcessSupervisor::nextTimeout() {
    std::scoped_lock lock(mutex_);
    auto timeout = options_.loopTick;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [name, when] : pendingRestarts_) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(when - now);
        timeout = std::min(timeout, std::max(remaining, std::chrono::milliseconds(0)));
    }
    return timeout;
}

Result<void> ProcessSupervisor::run() {
    if (!configured_) return makeError(ErrorCode::InvalidState, "no configuration loaded");
    if (auto r = SignalPipe::install(); !r) return r;

    if (!options_.controlSocketPath.empty()) {
        control_ = std::make_unique<ControlServer>(options_.controlSocketPath,
                                                   [this](const std::string& line) { return handleCommand(line); });
        if (auto r = control_->start(); !r) {
            logger().error(kComponent, "Control server failed: " + r.error().message);
            control_.reset();
            auto stopped = stopAll();
            (void)stopped;
            SignalPipe::uninstall();
            return r;
        }
    }
    monitor_->start();
    logger().info(kComponent, "Supervision loop running (Ctrl-C or SIGTERM for graceful shutdown)");

    while (!shutdownRequested_.load()) {
        const auto events = SignalPipe::wait(nextTimeout());
        for (const unsigned char ev : events) {
            if (ev == SIGINT || ev == SIGTERM) {
                logger().info(kComponent, std::string("Received ") + (ev == SIGINT ? "SIGINT" : "SIGTERM") +
                                           ", shutting down");
                shutdownRequested_.store(true);
            }
        }
        std::scoped_lock lock(mutex_);
        reapChildren();       // SIGCHLD may be coalesced or lost: always reap everything
        processDueRestarts();
    }

    // Graceful shutdown. Helper threads are joined WITHOUT holding mutex_ (they may be waiting for it).
    logger().info(kComponent, "Stopping services in reverse dependency order");
    monitor_->stop();
    auto result = stopAll();
    // The control socket stays up until every service is down, so `process-pilot stop`
    // (which waits for the socket to disappear) only returns after a complete shutdown.
    if (control_) {
        control_->stop();
        control_.reset();
    }
    SignalPipe::uninstall();
    logger().info(kComponent, "Shutdown complete");
    return result;
}

// ---------------------------------------------------------------- reporting & control

std::string ProcessSupervisor::statusTable() const {
    std::ostringstream out;
    out << std::left << std::setw(16) << "SERVICE" << std::setw(9) << "PID" << std::setw(20) << "STATE"
        << "RESTARTS\n";
    for (const ProcessInfo& p : registry_.snapshot()) {
        out << std::left << std::setw(16) << p.serviceName << std::setw(9)
            << (p.pid > 0 ? std::to_string(p.pid) : std::string("-")) << std::setw(20) << toString(p.state)
            << p.restartCount << '\n';
    }
    return out.str();
}

std::string ProcessSupervisor::monitorTable() {
    std::ostringstream out;
    if (auto snap = SysfsReader{}.snapshot(); snap) {
        out << "System: " << snap.value().onlineCpuCount << " CPU(s) online [" << snap.value().onlineCpuList << "]";
        if (snap.value().cpuFrequencyKhz) out << ", cpu0 " << *snap.value().cpuFrequencyKhz / 1000 << " MHz";
        if (!snap.value().thermalZones.empty()) {
            double hottest = snap.value().thermalZones.front().celsius;
            for (const auto& z : snap.value().thermalZones) hottest = std::max(hottest, z.celsius);
            out << std::fixed << std::setprecision(1) << ", hottest thermal zone " << hottest << " C";
        }
        out << '\n';
    }
    out << std::left << std::setw(16) << "SERVICE" << std::setw(9) << "PID" << std::setw(20) << "STATE"
        << std::setw(8) << "CPU%" << std::setw(11) << "RSS(MB)" << std::setw(9) << "THREADS" << std::setw(12)
        << "UPTIME" << "RESTARTS\n";

    const auto now = std::chrono::steady_clock::now();
    for (const ProcessInfo& p : registry_.snapshot()) {
        out << std::left << std::setw(16) << p.serviceName << std::setw(9)
            << (p.pid > 0 ? std::to_string(p.pid) : std::string("-")) << std::setw(20) << toString(p.state);
        bool filled = false;
        if (p.pid > 0 && p.state == ProcessState::Running && monitor_) {
            if (auto m = monitor_->inspect(p.pid); m) {
                std::ostringstream cpu;
                cpu << std::fixed << std::setprecision(1) << m.value().cpuPercent;
                std::ostringstream rss;
                rss << std::fixed << std::setprecision(1) << static_cast<double>(m.value().residentKb) / 1024.0;
                out << std::setw(8) << cpu.str() << std::setw(11) << rss.str() << std::setw(9) << m.value().threads
                    << std::setw(12)
                    << formatUptime(std::chrono::duration_cast<std::chrono::seconds>(now - p.startTime));
                filled = true;
            }
        }
        if (!filled) {
            out << std::setw(8) << "-" << std::setw(11) << "-" << std::setw(9) << "-" << std::setw(12) << "-";
        }
        out << p.restartCount << '\n';
    }
    return out.str();
}

std::string ProcessSupervisor::handleCommand(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    std::string arg;
    in >> cmd >> arg;

    const auto reply = [](const Result<void>& r, const std::string& okText) {
        return r ? "OK: " + okText + "\n" : "ERROR: " + r.error().describe() + "\n";
    };

    if (cmd == "ping") return "OK: pong\n";
    if (cmd == "status") return statusTable();
    if (cmd == "monitor") return monitorTable();
    if (cmd == "stop") {
        requestShutdown();
        return "OK: shutdown requested\n";
    }
    if (cmd == "restart") {
        if (arg.empty()) return reply(restartAll(), "all services restarted");
        return reply(restartService(arg), "restarted " + arg);
    }
    return "ERROR: unknown command '" + cmd + "'\n";
}

}  // namespace pilot
