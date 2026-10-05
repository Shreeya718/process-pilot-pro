#pragma once

#include <sys/types.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/result.hpp"
#include "process/process_info.hpp"

namespace pilot {

/// Service -> ProcessInfo and PID -> service lookup tables.
///
/// Thread safety: every public method takes the internal mutex, which is a
/// *leaf* lock: nothing is called while it is held, so it can never take part
/// in a lock-order cycle. Getters return copies, so callers never hold
/// references into the maps (no iterator/reference invalidation).
class ProcessRegistry {
public:
    [[nodiscard]] Result<void> registerService(const std::string& name);

    [[nodiscard]] bool contains(const std::string& name) const;
    [[nodiscard]] std::optional<ProcessInfo> get(const std::string& name) const;
    [[nodiscard]] std::optional<std::string> findByPid(pid_t pid) const;

    /// Validated state change (see isValidTransition).
    [[nodiscard]] Result<void> transition(const std::string& name, ProcessState next);

    /// Starting -> Running, records the PID and start time.
    [[nodiscard]] Result<void> markRunning(const std::string& name, pid_t pid,
                                           std::chrono::steady_clock::time_point startedAt);

    /// Forget the PID of a service whose process has been reaped.
    void clearPid(const std::string& name);

    [[nodiscard]] Result<void> incrementRestartCount(const std::string& name);

    /// Copy of all entries in registration order.
    [[nodiscard]] std::vector<ProcessInfo> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::string> order_;
    std::unordered_map<std::string, ProcessInfo> byName_;
    std::unordered_map<pid_t, std::string> pidIndex_;
};

}  // namespace pilot
