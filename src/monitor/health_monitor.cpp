#include "monitor/health_monitor.hpp"

#include <unistd.h>

#include <algorithm>
#include <unordered_set>

#include "logging/logger.hpp"

namespace pilot {

std::string_view toString(HealthIssueKind kind) noexcept {
    switch (kind) {
        case HealthIssueKind::ProcessMissing: return "process missing";
        case HealthIssueKind::ZombieProcess: return "zombie process";
        case HealthIssueKind::Suspended: return "process suspended";
        case HealthIssueKind::Reparented: return "unexpected parent";
        case HealthIssueKind::MemoryLimitExceeded: return "memory limit exceeded";
    }
    return "unknown";
}

std::optional<HealthIssue> evaluateHealth(const std::string& service, const ProcessMetrics& m,
                                          const HealthSettings& settings, pid_t expectedParent) {
    const auto issue = [&](HealthIssueKind kind, std::string detail) {
        return std::optional<HealthIssue>(HealthIssue{service, m.pid, kind, std::move(detail)});
    };

    if (m.state == 'Z' || m.state == 'X' || m.state == 'x') {
        return issue(HealthIssueKind::ZombieProcess, std::string("kernel state '") + m.state + "'");
    }
    if (m.state == 'T' || m.state == 't') {
        return issue(HealthIssueKind::Suspended, std::string("kernel state '") + m.state + "'");
    }
    if (expectedParent > 0 && m.parentPid != expectedParent) {
        return issue(HealthIssueKind::Reparented,
                     "parent pid is " + std::to_string(m.parentPid) + ", expected " + std::to_string(expectedParent));
    }
    if (settings.maxRssKb > 0 && m.residentKb > settings.maxRssKb) {
        return issue(HealthIssueKind::MemoryLimitExceeded,
                     "RSS " + std::to_string(m.residentKb) + " kB > limit " + std::to_string(settings.maxRssKb) + " kB");
    }
    return std::nullopt;
}

HealthMonitor::HealthMonitor(const ProcessRegistry& registry, ProcReader reader, IssueCallback callback)
    : registry_(registry),
      reader_(std::move(reader)),
      callback_(std::move(callback)),
      ticksPerSecond_(std::max(1L, ::sysconf(_SC_CLK_TCK))) {}

HealthMonitor::~HealthMonitor() { stop(); }

void HealthMonitor::configure(std::unordered_map<std::string, HealthSettings> settings) {
    std::scoped_lock lock(mutex_);
    settings_ = std::move(settings);
}

void HealthMonitor::start() {
    std::scoped_lock lock(mutex_);
    if (thread_.joinable()) {
        return;
    }
    stopRequested_ = false;
    thread_ = std::thread(&HealthMonitor::run, this);
}

void HealthMonitor::stop() {
    {
        std::scoped_lock lock(mutex_);
        stopRequested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

Result<ProcessMetrics> HealthMonitor::inspect(pid_t pid) {
    auto read = reader_.readProcess(pid);
    if (!read) {
        return read;
    }
    ProcessMetrics metrics = std::move(read.value());

    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(sampleMutex_);
    const auto it = samples_.find(pid);
    if (it != samples_.end()) {
        const double seconds = std::chrono::duration<double>(now - it->second.at).count();
        if (seconds > 0.0 && metrics.totalCpuTicks() >= it->second.ticks) {
            const double cpuSeconds =
                static_cast<double>(metrics.totalCpuTicks() - it->second.ticks) / static_cast<double>(ticksPerSecond_);
            metrics.cpuPercent = 100.0 * cpuSeconds / seconds;
        }
    }
    samples_[pid] = CpuSample{metrics.totalCpuTicks(), now};
    return metrics;
}

void HealthMonitor::run() {
    std::unique_lock lock(mutex_);
    while (!stopRequested_) {
        // Wake up at the shortest configured interval (bounded to a sane range).
        auto tick = std::chrono::milliseconds(1000);
        for (const auto& [name, s] : settings_) {
            tick = std::min(tick, s.interval);
        }
        tick = std::max(tick, std::chrono::milliseconds(50));

        cv_.wait_for(lock, tick, [this] { return stopRequested_; });
        if (stopRequested_) {
            break;
        }
        lock.unlock();
        checkAll();
        lock.lock();
    }
}

void HealthMonitor::checkAll() {
    std::unordered_map<std::string, HealthSettings> settings;
    {
        std::scoped_lock lock(mutex_);
        settings = settings_;
    }

    const auto now = std::chrono::steady_clock::now();
    const pid_t self = ::getpid();
    std::unordered_set<pid_t> livePids;

    for (const ProcessInfo& info : registry_.snapshot()) {
        if (info.state != ProcessState::Running || info.pid <= 0) {
            continue;
        }
        livePids.insert(info.pid);

        const auto cfg = settings.find(info.serviceName);
        if (cfg == settings.end()) {
            continue;  // health checks disabled for this service
        }
        const auto last = lastChecked_.find(info.serviceName);
        if (last != lastChecked_.end() && now - last->second < cfg->second.interval) {
            continue;  // not due yet
        }
        lastChecked_[info.serviceName] = now;

        auto metrics = inspect(info.pid);
        std::optional<HealthIssue> issue;
        if (!metrics) {
            if (metrics.error().code == ErrorCode::ProcessNotFound) {
                issue = HealthIssue{info.serviceName, info.pid, HealthIssueKind::ProcessMissing,
                                    metrics.error().message};
            } else {
                // Cannot read /proc (permissions, transient error): not evidence of ill health.
                Logger::instance().debug("HealthMonitor", "cannot inspect " + info.serviceName + ": " +
                                                              metrics.error().describe(), info.pid);
            }
        } else {
            issue = evaluateHealth(info.serviceName, metrics.value(), cfg->second, self);
        }

        if (issue && callback_) {
            callback_(*issue);
        }
    }

    // Forget CPU samples of processes that are gone.
    std::scoped_lock lock(sampleMutex_);
    for (auto it = samples_.begin(); it != samples_.end();) {
        it = livePids.contains(it->first) ? std::next(it) : samples_.erase(it);
    }
}

}  // namespace pilot
