#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/result.hpp"
#include "config/service_config.hpp"
#include "monitor/health_monitor.hpp"
#include "process/process_manager.hpp"
#include "process/process_registry.hpp"
#include "restart/restart_policy.hpp"
#include "supervisor/control_server.hpp"

namespace pilot {

struct SupervisorOptions {
    /// Control socket for `process-pilot status|stop|...`; empty = no control server.
    std::filesystem::path controlSocketPath;
    /// Upper bound for one wait in the main loop (safety net; the loop is event driven).
    std::chrono::milliseconds loopTick{1000};
};

/// Orchestration layer:
///   Configuration -> DependencyGraph -> ProcessManager -> ProcessRegistry
///   -> HealthMonitor -> RestartPolicy -> Logger
///
/// Threads
///   main thread    : run() event loop (signals via SignalPipe, due restarts, reaping)
///   health thread  : HealthMonitor, reports unhealthy services through onHealthIssue()
///   control thread : ControlServer, executes CLI commands (status/restart/stop/...)
///
/// Locking (see docs/architecture.md)
///   mutex_  - "operation lock": serialises every lifecycle operation
///             (start/stop/restart, child reaping, restart scheduling) and guards
///             policies_ / pendingRestarts_.
///   ProcessRegistry::mutex_ - leaf lock inside the registry.
///   Order: mutex_ -> registry mutex. The registry never calls back, so no cycles.
///   Read-only status queries use only the registry lock, so `status` stays
///   responsive even while a slow stop is in progress.
///   services_/startOrder_/stopOrder_ are written once in loadDefinitions()
///   (before any thread starts) and are immutable afterwards.
class ProcessSupervisor {
public:
    explicit ProcessSupervisor(SupervisorOptions options = SupervisorOptions{});
    ~ProcessSupervisor();

    ProcessSupervisor(const ProcessSupervisor&) = delete;
    ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

    [[nodiscard]] Result<void> loadConfiguration(const std::filesystem::path& path);
    [[nodiscard]] Result<void> loadDefinitions(std::vector<ServiceDefinition> definitions);

    /// Install SIGINT/SIGTERM/SIGCHLD handling (call before startAll() so no signal is lost).
    [[nodiscard]] Result<void> installSignalHandlers();

    /// Start every enabled service in dependency order; rolls back on failure.
    [[nodiscard]] Result<void> startAll();

    /// Stop every service in reverse dependency order.
    [[nodiscard]] Result<void> stopAll();

    /// Supervision loop; returns after SIGINT/SIGTERM/requestShutdown() and a graceful stopAll().
    [[nodiscard]] Result<void> run();

    [[nodiscard]] Result<void> restartService(const std::string& serviceName);
    [[nodiscard]] Result<void> restartAll();

    /// Thread- and signal-context safe way to end run().
    void requestShutdown() noexcept;

    [[nodiscard]] std::vector<ProcessInfo> status() const { return registry_.snapshot(); }
    [[nodiscard]] const std::vector<std::string>& startupOrder() const noexcept { return startOrder_; }
    [[nodiscard]] const std::vector<std::string>& shutdownOrder() const noexcept { return stopOrder_; }

    [[nodiscard]] std::string statusTable() const;
    [[nodiscard]] std::string monitorTable();

private:
    // ---- all of these require mutex_ to be held by the caller ----
    Result<void> startService(const std::string& name);
    Result<void> stopService(const std::string& name);
    Result<void> checkDependenciesRunning(const std::string& name) const;
    void reapChildren();
    void handleExit(const ProcessExitStatus& status);
    void applyExitPolicy(const std::string& name, bool abnormal, const std::string& what);
    void processDueRestarts();
    void forceState(const std::string& name, ProcessState state);
    // --------------------------------------------------------------
    void onHealthIssue(const HealthIssue& issue);
    std::string handleCommand(const std::string& line);
    std::chrono::milliseconds nextTimeout();

    SupervisorOptions options_;
    mutable std::mutex mutex_;

    std::unordered_map<std::string, ServiceDefinition> services_;
    std::vector<std::string> startOrder_;  // enabled services, dependencies first
    std::vector<std::string> stopOrder_;   // reverse of startOrder_

    ProcessRegistry registry_;
    ProcessManager manager_;
    std::unordered_map<std::string, RestartPolicy> policies_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pendingRestarts_;

    std::unique_ptr<HealthMonitor> monitor_;
    std::unique_ptr<ControlServer> control_;

    std::atomic<bool> shutdownRequested_{false};
    bool configured_{false};
};

}  // namespace pilot
