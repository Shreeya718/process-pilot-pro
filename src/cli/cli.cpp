#include "cli/cli.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "config/config_parser.hpp"
#include "dependency/dependency_graph.hpp"
#include "logging/logger.hpp"
#include "supervisor/control_server.hpp"
#include "supervisor/supervisor.hpp"

namespace pilot {

namespace {

constexpr const char* kVersion = "1.0.0";

struct CliOptions {
    std::string command;
    std::vector<std::string> positional;
    std::filesystem::path socket = defaultSocketPath();
    std::filesystem::path logFile;
    LogLevel logLevel{LogLevel::Info};
    long intervalMs{1000};
    long count{0};  // 0 = until interrupted
    bool help{false};
    bool version{false};
};

void printUsage(std::ostream& out) {
    out << "Process Pilot Pro " << kVersion << " - Linux process supervisor with dependency resolution,\n"
        << "health monitoring and self-healing.\n\n"
        << "Usage: process-pilot <command> [arguments] [options]\n\n"
        << "Commands:\n"
        << "  validate <config>      Parse and validate a configuration; print the startup order\n"
        << "  list     <config>      List configured services with dependencies and policies\n"
        << "  start    <config>      Start all services and supervise them (foreground)\n"
        << "  stop                   Gracefully stop the running supervisor and its services\n"
        << "  restart  [service]     Restart one service (or all) in the running supervisor\n"
        << "  status                 Show PID / state / restart count of every service\n"
        << "  monitor                Live CPU / memory / uptime view (reads /proc)\n\n"
        << "Options:\n"
        << "  --socket PATH          Control socket (default: " << defaultSocketPath().string() << ")\n"
        << "  --log-file PATH        Also write the log to PATH\n"
        << "  --log-level LEVEL      debug | info | warning | error | critical (default: info)\n"
        << "  --interval MS          monitor refresh interval (default: 1000)\n"
        << "  --count N              monitor: stop after N refreshes (default: unlimited)\n"
        << "  -h, --help             Show this help\n"
        << "  --version              Show version\n";
}

bool parseLong(const std::string& text, long& out) {
    long v = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec != std::errc() || ptr != text.data() + text.size()) return false;
    out = v;
    return true;
}

/// Returns an error message, or an empty string on success.
std::string parseArgs(int argc, char** argv, CliOptions& opts) {
    std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string arg = args[i];
        std::string inlineValue;
        bool hasInline = false;
        if (arg.rfind("--", 0) == 0) {
            if (const auto eq = arg.find('='); eq != std::string::npos) {
                inlineValue = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
                hasInline = true;
            }
        }
        const auto value = [&](std::string& out) -> bool {
            if (hasInline) {
                out = inlineValue;
                return true;
            }
            if (i + 1 >= args.size()) return false;
            out = args[++i];
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            opts.help = true;
        } else if (arg == "--version") {
            opts.version = true;
        } else if (arg == "--socket" || arg == "--log-file" || arg == "--log-level" || arg == "--interval" ||
                   arg == "--count") {
            std::string v;
            if (!value(v)) return "option " + arg + " requires a value";
            if (arg == "--socket") {
                opts.socket = v;
            } else if (arg == "--log-file") {
                opts.logFile = v;
            } else if (arg == "--log-level") {
                const auto level = parseLogLevel(v);
                if (!level) return "unknown log level '" + v + "'";
                opts.logLevel = *level;
            } else {
                long n = 0;
                if (!parseLong(v, n) || n < 0) return arg + " expects a non-negative integer";
                if (arg == "--interval") {
                    if (n < 100) return "--interval must be at least 100";
                    opts.intervalMs = n;
                } else {
                    opts.count = n;
                }
            }
        } else if (arg.rfind("-", 0) == 0 && arg.size() > 1) {
            return "unknown option '" + arg + "'";
        } else if (opts.command.empty()) {
            opts.command = arg;
        } else {
            opts.positional.push_back(arg);
        }
    }
    return {};
}

int fail(const std::string& message) {
    std::cerr << "ERROR: " << message << '\n';
    return 1;
}

int usageError(const std::string& message) {
    std::cerr << "ERROR: " << message << "\n\n";
    printUsage(std::cerr);
    return 2;
}

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += (i == 0 ? "" : ", ") + names[i];
    }
    return out.empty() ? "-" : out;
}

Result<std::vector<ServiceDefinition>> loadDefinitions(const std::string& path) {
    return ConfigParser{}.parseFile(path);
}

int cmdValidate(const CliOptions& opts) {
    if (opts.positional.size() != 1) return usageError("validate expects exactly one configuration file");
    auto defs = loadDefinitions(opts.positional[0]);
    if (!defs) return fail(defs.error().message);

    DependencyGraph graph;
    for (const auto& d : defs.value()) {
        if (auto r = graph.addService(d); !r) return fail(r.error().message);
    }
    auto order = graph.topologicalOrder();
    if (!order) return fail(order.error().message);

    std::unordered_map<std::string, const ServiceDefinition*> byName;
    for (const auto& d : defs.value()) byName[d.name] = &d;
    std::vector<std::string> startup;
    for (const auto& n : order.value()) {
        if (byName.at(n)->enabled) startup.push_back(n);
    }
    std::cout << "Configuration OK: " << defs.value().size() << " service(s), " << startup.size() << " enabled\n"
              << "Startup order:\n";
    for (std::size_t i = 0; i < startup.size(); ++i) std::cout << "  " << (i + 1) << ". " << startup[i] << '\n';
    std::cout << "Shutdown order:\n";
    for (std::size_t i = 0; i < startup.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << startup[startup.size() - 1 - i] << '\n';
    }
    return 0;
}

int cmdList(const CliOptions& opts) {
    if (opts.positional.empty()) {
        auto reply = sendControlCommand(opts.socket, "status", std::chrono::milliseconds(5000));
        if (!reply) return fail(reply.error().message);
        std::cout << reply.value();
        return 0;
    }
    auto defs = loadDefinitions(opts.positional[0]);
    if (!defs) return fail(defs.error().message);
    DependencyGraph graph;
    for (const auto& d : defs.value()) {
        if (auto r = graph.addService(d); !r) return fail(r.error().message);
    }
    auto order = graph.topologicalOrder();
    if (!order) return fail(order.error().message);

    std::unordered_map<std::string, const ServiceDefinition*> byName;
    for (const auto& d : defs.value()) byName[d.name] = &d;

    std::cout << std::left << std::setw(7) << "ORDER" << std::setw(16) << "SERVICE" << std::setw(24)
              << "DEPENDS ON" << std::setw(12) << "RESTART" << std::setw(9) << "ENABLED" << "EXECUTABLE\n";
    std::size_t index = 1;
    for (const auto& name : order.value()) {
        const ServiceDefinition& d = *byName.at(name);
        std::cout << std::left << std::setw(7) << index++ << std::setw(16) << d.name << std::setw(24)
                  << joinNames(d.dependencies) << std::setw(12) << toString(d.restart.mode) << std::setw(9)
                  << (d.enabled ? "yes" : "no") << d.executable << '\n';
    }
    return 0;
}

int cmdStart(const CliOptions& opts) {
    if (opts.positional.size() != 1) return usageError("start expects exactly one configuration file");

    if (sendControlCommand(opts.socket, "ping", std::chrono::milliseconds(1000))) {
        return fail("a supervisor is already running (socket " + opts.socket.string() + ")");
    }

    SupervisorOptions so;
    so.controlSocketPath = opts.socket;
    ProcessSupervisor supervisor(so);

    if (auto r = supervisor.loadConfiguration(opts.positional[0]); !r) return fail(r.error().message);
    if (auto r = supervisor.installSignalHandlers(); !r) return fail(r.error().message);

    Logger::instance().info("Supervisor", "Startup order: " + joinNames(supervisor.startupOrder()));
    if (auto r = supervisor.startAll(); !r) return fail(r.error().message);

    if (auto r = supervisor.run(); !r) return fail(r.error().message);
    return 0;
}

int cmdRemote(const CliOptions& opts, const std::string& request, std::chrono::milliseconds timeout) {
    auto reply = sendControlCommand(opts.socket, request, timeout);
    if (!reply) {
        std::cerr << "ERROR: " << reply.error().message
                  << "\n(start a supervisor with: process-pilot start <config>)\n";
        return 1;
    }
    std::cout << reply.value();
    return reply.value().rfind("ERROR", 0) == 0 ? 1 : 0;
}

int cmdStop(const CliOptions& opts) {
    const int rc = cmdRemote(opts, "stop", std::chrono::milliseconds(60000));
    if (rc != 0) return rc;
    // Wait until the supervisor has really gone away (it removes its socket on exit).
    for (int i = 0; i < 600; ++i) {
        if (!sendControlCommand(opts.socket, "ping", std::chrono::milliseconds(500))) {
            std::cout << "Supervisor stopped.\n";
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return fail("supervisor did not stop within 60 seconds");
}

int cmdMonitor(const CliOptions& opts) {
    for (long i = 0; opts.count == 0 || i < opts.count; ++i) {
        auto reply = sendControlCommand(opts.socket, "monitor", std::chrono::milliseconds(5000));
        if (!reply) return fail(reply.error().message);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char stamp[32] = {0};
        std::tm tm{};
        localtime_r(&now, &tm);
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
        std::cout << "--- " << stamp << " ---\n" << reply.value() << std::flush;
        if (opts.count != 0 && i + 1 >= opts.count) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.intervalMs));
    }
    return 0;
}

}  // namespace

int Cli::run(int argc, char** argv) {
    CliOptions opts;
    if (const std::string err = parseArgs(argc, argv, opts); !err.empty()) return usageError(err);

    if (opts.version) {
        std::cout << "process-pilot " << kVersion << '\n';
        return 0;
    }
    if (opts.help || opts.command.empty() || opts.command == "help") {
        printUsage(std::cout);
        return (opts.help || opts.command == "help") ? 0 : 2;
    }

    Logger::instance().setLevel(opts.logLevel);
    if (!opts.logFile.empty()) {
        if (auto r = Logger::instance().setLogFile(opts.logFile); !r) return fail(r.error().message);
    }

    const std::string& cmd = opts.command;
    if (cmd == "validate") return cmdValidate(opts);
    if (cmd == "list") return cmdList(opts);
    if (cmd == "start") return cmdStart(opts);
    if (cmd == "stop") return cmdStop(opts);
    if (cmd == "status") return cmdRemote(opts, "status", std::chrono::milliseconds(5000));
    if (cmd == "monitor") return cmdMonitor(opts);
    if (cmd == "restart") {
        if (opts.positional.size() > 1) return usageError("restart takes at most one service name");
        const std::string request = opts.positional.empty() ? "restart" : "restart " + opts.positional[0];
        return cmdRemote(opts, request, std::chrono::milliseconds(120000));
    }
    return usageError("unknown command '" + cmd + "'");
}

}  // namespace pilot
