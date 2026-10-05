#pragma once

#include <sys/types.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "common/result.hpp"
#include "linux/proc_reader.hpp"
#include "monitor/process_metrics.hpp"
#include "process/process_registry.hpp"

namespace pilot {

enum class HealthIssueKind {
    ProcessMissing,       // /proc/<pid> vanished although the registry says RUNNING
    ZombieProcess,        // kernel state 'Z'
    Suspended,            // kernel state 'T'/'t' (stopped)
    Reparented,           // PPid != supervisor: PID was reused or the child was orphaned
    MemoryLimitExceeded   // VmRSS above max_rss_kb
};

std::string_view toString(HealthIssueKind kind) noexcept;

struct HealthIssue {
    std::string service;
    pid_t pid{0};
    HealthIssueKind kind{HealthIssueKind::ProcessMissing};
    std::string detail;
};

struct HealthSettings {
    std::chrono::milliseconds interval{2000};
    unsigned long maxRssKb{0};  // 0 = unlimited
};

/// Pure health rules, separated from I/O so they can be unit tested.
/// Levels: (1) kernel state, (2) ownership (parent pid), (3) resource limits.
/// (Level 0 - existence - is decided by whether /proc/<pid> can be read.)
std::optional<HealthIssue> evaluateHealth(const std::string& service, const ProcessMetrics& metrics,
                                          const HealthSettings& settings, pid_t expectedParent);

/// Background thread that periodically inspects RUNNING services through /proc.
///
/// Threading: exactly one worker thread. It sleeps on a condition variable
/// (timed wait) - no busy loop - and is woken immediately by stop().
/// Locks: `mutex_` guards only the stop flag / settings; `sampleMutex_` guards
/// CPU samples. Neither is held while the registry is read or the callback is
/// invoked, so the callback may freely take the supervisor's lock.
/// IMPORTANT: stop() joins the thread, so it must not be called while holding
/// any lock the callback needs.
class HealthMonitor {
public:
    using IssueCallback = std::function<void(const HealthIssue&)>;

    HealthMonitor(const ProcessRegistry& registry, ProcReader reader, IssueCallback callback);
    ~HealthMonitor();

    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;

    /// Per-service settings (services without an entry are not monitored). Call before start().
    void configure(std::unordered_map<std::string, HealthSettings> settings);

    void start();
    void stop();

    /// Read /proc for `pid` and fill cpuPercent from the previous sample of that pid.
    [[nodiscard]] Result<ProcessMetrics> inspect(pid_t pid);

private:
    struct CpuSample {
        unsigned long long ticks{0};
        std::chrono::steady_clock::time_point at{};
    };

    void run();
    void checkAll();

    const ProcessRegistry& registry_;
    ProcReader reader_;
    IssueCallback callback_;
    const long ticksPerSecond_;

    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopRequested_{false};
    std::unordered_map<std::string, HealthSettings> settings_;
    std::thread thread_;

    std::mutex sampleMutex_;
    std::unordered_map<pid_t, CpuSample> samples_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastChecked_;  // worker thread only
};

}  // namespace pilot
