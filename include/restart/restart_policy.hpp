#pragma once

#include <chrono>
#include <deque>
#include <string>

#include "config/service_config.hpp"

namespace pilot {

enum class RestartAction {
    DoNotRestart,  // policy says leave it down (never / clean exit under on_failure)
    Restart,       // restart after `delay`
    GiveUp         // crash loop: restart budget exhausted -> PERMANENTLY_FAILED
};

struct RestartDecision {
    RestartAction action{RestartAction::DoNotRestart};
    std::chrono::milliseconds delay{0};
    unsigned failuresInWindow{0};
    std::string reason;
};

/// Decides whether an exited service is restarted and tracks crash loops.
///
/// Algorithm (sliding window): every exit that the mode considers a failure is
/// timestamped. Timestamps older than `window` are discarded. If more than
/// `maxRestarts` remain, the service has crash-looped -> GiveUp.
/// A window of 0 seconds means "count failures over the whole lifetime".
///
/// Not thread-safe by itself: the ProcessSupervisor serialises all calls.
class RestartPolicy {
public:
    explicit RestartPolicy(RestartConfig config = RestartConfig{});

    /// Record an exit of the service. `abnormal` = non-zero exit code or killed by a signal.
    [[nodiscard]] RestartDecision recordExit(bool abnormal, std::chrono::steady_clock::time_point now);

    /// Forget the failure history (used on manual restarts).
    void reset() noexcept { history_.clear(); }

    [[nodiscard]] const RestartConfig& config() const noexcept { return config_; }

private:
    void prune(std::chrono::steady_clock::time_point now);

    RestartConfig config_;
    std::deque<std::chrono::steady_clock::time_point> history_;
};

}  // namespace pilot
