#include "restart/restart_policy.hpp"
#include "test_framework.hpp"

using namespace pilot;
using namespace std::chrono_literals;

namespace {
RestartConfig cfg(RestartMode mode, unsigned max, std::chrono::seconds window) {
    RestartConfig c;
    c.mode = mode;
    c.maxRestarts = max;
    c.delay = 250ms;
    c.window = window;
    return c;
}
const auto t0 = std::chrono::steady_clock::time_point{} + 1000s;
}  // namespace

PP_TEST(never_does_not_restart) {
    RestartPolicy p(cfg(RestartMode::Never, 5, 60s));
    CHECK(p.recordExit(true, t0).action == RestartAction::DoNotRestart);
    CHECK(p.recordExit(false, t0).action == RestartAction::DoNotRestart);
}

PP_TEST(on_failure_ignores_clean_exit) {
    RestartPolicy p(cfg(RestartMode::OnFailure, 5, 60s));
    CHECK(p.recordExit(false, t0).action == RestartAction::DoNotRestart);
    const auto d = p.recordExit(true, t0);
    CHECK(d.action == RestartAction::Restart);
    CHECK(d.delay == 250ms);
}

PP_TEST(always_restarts_clean_exits_too) {
    RestartPolicy p(cfg(RestartMode::Always, 5, 60s));
    CHECK(p.recordExit(false, t0).action == RestartAction::Restart);
}

PP_TEST(gives_up_after_max_restarts) {
    RestartPolicy p(cfg(RestartMode::OnFailure, 3, 0s));  // lifetime counting
    CHECK(p.recordExit(true, t0).action == RestartAction::Restart);
    CHECK(p.recordExit(true, t0 + 1s).action == RestartAction::Restart);
    CHECK(p.recordExit(true, t0 + 2s).action == RestartAction::Restart);
    const auto d = p.recordExit(true, t0 + 3s);
    CHECK(d.action == RestartAction::GiveUp);
    CHECK(d.failuresInWindow == 4u);
    CHECK(!d.reason.empty());
}

PP_TEST(crash_loop_window_forgives_old_failures) {
    RestartPolicy p(cfg(RestartMode::OnFailure, 2, 60s));
    CHECK(p.recordExit(true, t0).action == RestartAction::Restart);
    CHECK(p.recordExit(true, t0 + 10s).action == RestartAction::Restart);
    // 100 s later the first two failures have left the window.
    CHECK(p.recordExit(true, t0 + 110s).action == RestartAction::Restart);
    CHECK(p.recordExit(true, t0 + 111s).action == RestartAction::Restart);
    // Third failure inside the 60 s window -> crash loop.
    CHECK(p.recordExit(true, t0 + 112s).action == RestartAction::GiveUp);
}

PP_TEST(zero_max_restarts_gives_up_immediately) {
    RestartPolicy p(cfg(RestartMode::OnFailure, 0, 60s));
    CHECK(p.recordExit(true, t0).action == RestartAction::GiveUp);
}

PP_TEST(reset_clears_history) {
    RestartPolicy p(cfg(RestartMode::OnFailure, 1, 0s));
    CHECK(p.recordExit(true, t0).action == RestartAction::Restart);
    CHECK(p.recordExit(true, t0).action == RestartAction::GiveUp);
    p.reset();
    CHECK(p.recordExit(true, t0).action == RestartAction::Restart);
}

PP_TEST_MAIN()
