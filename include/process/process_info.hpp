#pragma once

#include <sys/types.h>

#include <chrono>
#include <string>

#include "process/process_state.hpp"

namespace pilot {

/// Runtime record of one service (the value stored in ProcessRegistry).
struct ProcessInfo {
    std::string serviceName;
    pid_t pid{0};  // 0 = no live process
    ProcessState state{ProcessState::Stopped};
    int restartCount{0};
    std::chrono::steady_clock::time_point startTime{};
};

}  // namespace pilot
