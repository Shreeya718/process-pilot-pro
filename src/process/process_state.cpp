#include "process/process_state.hpp"

namespace pilot {

std::string_view toString(ProcessState state) noexcept {
    switch (state) {
        case ProcessState::Stopped: return "STOPPED";
        case ProcessState::Starting: return "STARTING";
        case ProcessState::Running: return "RUNNING";
        case ProcessState::Stopping: return "STOPPING";
        case ProcessState::Failed: return "FAILED";
        case ProcessState::Restarting: return "RESTARTING";
        case ProcessState::PermanentlyFailed: return "PERMANENTLY_FAILED";
    }
    return "UNKNOWN";
}

bool isValidTransition(ProcessState from, ProcessState to) noexcept {
    switch (from) {
        case ProcessState::Stopped:
            return to == ProcessState::Starting;
        case ProcessState::Starting:
            return to == ProcessState::Running || to == ProcessState::Failed;
        case ProcessState::Running:
            // Running -> Stopped = clean exit that the restart policy does not revive.
            return to == ProcessState::Stopping || to == ProcessState::Failed || to == ProcessState::Stopped;
        case ProcessState::Stopping:
            return to == ProcessState::Stopped;
        case ProcessState::Failed:
            // Starting = manual restart, Stopped = operator stop.
            return to == ProcessState::Restarting || to == ProcessState::PermanentlyFailed ||
                   to == ProcessState::Starting || to == ProcessState::Stopped;
        case ProcessState::Restarting:
            return to == ProcessState::Starting || to == ProcessState::Stopped;
        case ProcessState::PermanentlyFailed:
            return to == ProcessState::Starting || to == ProcessState::Stopped;
    }
    return false;
}

}  // namespace pilot
