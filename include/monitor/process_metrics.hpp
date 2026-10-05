#pragma once

#include <sys/types.h>

#include <string>

namespace pilot {

/// Snapshot of one process as exposed by /proc/<pid>/{stat,status,cmdline}.
struct ProcessMetrics {
    pid_t pid{0};
    pid_t parentPid{0};
    char state{'?'};                       // R S D Z T t X ... (kernel one-letter state)
    std::string name;                      // comm (executable name, max 15 chars)
    std::string commandLine;               // argv joined by spaces (empty for kernel threads/zombies)
    unsigned long long userTicks{0};       // utime  (clock ticks)
    unsigned long long systemTicks{0};     // stime  (clock ticks)
    unsigned long long startTimeTicks{0};  // starttime since boot (clock ticks)
    unsigned long residentKb{0};           // VmRSS
    unsigned long virtualKb{0};            // VmSize
    long threads{0};
    double cpuPercent{0.0};                // filled in by HealthMonitor from two samples

    [[nodiscard]] unsigned long long totalCpuTicks() const noexcept { return userTicks + systemTicks; }
};

}  // namespace pilot
