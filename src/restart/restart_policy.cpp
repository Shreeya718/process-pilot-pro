#include "restart/restart_policy.hpp"

namespace pilot {

RestartPolicy::RestartPolicy(RestartConfig config) : config_(config) {}

void RestartPolicy::prune(std::chrono::steady_clock::time_point now) {
    if (config_.window.count() <= 0) {
        return;  // lifetime counting
    }
    while (!history_.empty() && now - history_.front() > config_.window) {
        history_.pop_front();
    }
}

RestartDecision RestartPolicy::recordExit(bool abnormal, std::chrono::steady_clock::time_point now) {
    RestartDecision decision;

    if (config_.mode == RestartMode::Never) {
        decision.action = RestartAction::DoNotRestart;
        decision.reason = "restart policy is 'never'";
        return decision;
    }
    if (config_.mode == RestartMode::OnFailure && !abnormal) {
        decision.action = RestartAction::DoNotRestart;
        decision.reason = "clean exit and restart policy is 'on_failure'";
        return decision;
    }

    prune(now);
    history_.push_back(now);
    decision.failuresInWindow = static_cast<unsigned>(history_.size());

    if (history_.size() > config_.maxRestarts) {
        decision.action = RestartAction::GiveUp;
        decision.reason = std::to_string(history_.size()) + " failures exceed max_restarts=" +
                          std::to_string(config_.maxRestarts) +
                          (config_.window.count() > 0
                               ? " within " + std::to_string(config_.window.count()) + "s"
                               : std::string(" (lifetime)"));
        return decision;
    }

    decision.action = RestartAction::Restart;
    decision.delay = config_.delay;
    decision.reason = "failure " + std::to_string(history_.size()) + " of max " +
                      std::to_string(config_.maxRestarts);
    return decision;
}

}  // namespace pilot
