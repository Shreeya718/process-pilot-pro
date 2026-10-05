#pragma once

#include <chrono>
#include <vector>

#include "common/result.hpp"

namespace pilot {

/// Async-signal-safe bridge from signal handlers to the supervisor loop
/// (the classic "self-pipe trick").
///
/// The handler does exactly one thing: write() one byte (the signal number)
/// into a non-blocking pipe - write() is async-signal-safe, malloc/iostream/
/// mutexes are not. The main loop poll()s the read end and does all real work
/// (waitpid, logging, state changes) in normal thread context.
///
/// This is process-wide state (signal dispositions are process-wide), so only
/// one supervisor loop may use it at a time. Event byte 0 is a plain wake-up.
class SignalPipe {
public:
    /// Installs handlers for SIGINT, SIGTERM and SIGCHLD (idempotent).
    [[nodiscard]] static Result<void> install();

    /// Restores default dispositions and closes the pipe.
    static void uninstall();

    [[nodiscard]] static bool installed();

    /// Wake the loop (async-signal-safe, usable from any thread; no-op if not installed).
    static void notify(unsigned char event = 0) noexcept;

    /// Block up to `timeout` for events; returns every pending event byte.
    [[nodiscard]] static std::vector<unsigned char> wait(std::chrono::milliseconds timeout);
};

}  // namespace pilot
