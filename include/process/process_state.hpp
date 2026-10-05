#pragma once

#include <string_view>

namespace pilot {

/// Lifecycle of a supervised service.
///
///   Stopped -> Starting -> Running -> Stopping -> Stopped
///   Running -> Failed -> Restarting -> Starting -> Running
///   Failed  -> PermanentlyFailed          (restart budget exhausted)
enum class ProcessState {
    Stopped,
    Starting,
    Running,
    Stopping,
    Failed,
    Restarting,
    PermanentlyFailed
};

/// Upper-case display name, e.g. "PERMANENTLY_FAILED".
std::string_view toString(ProcessState state) noexcept;

/// True when the state machine allows `from -> to`.
bool isValidTransition(ProcessState from, ProcessState to) noexcept;

}  // namespace pilot
