#include <chrono>

#include "process/process_registry.hpp"
#include "test_framework.hpp"

using namespace pilot;

PP_TEST(register_and_lookup) {
    ProcessRegistry reg;
    CHECK(reg.registerService("A").ok());
    CHECK(!reg.registerService("A").ok());  // duplicate
    CHECK(reg.contains("A"));
    CHECK(!reg.contains("B"));
    auto info = reg.get("A");
    CHECK(info.has_value());
    if (info) {
        CHECK(info->state == ProcessState::Stopped);
        CHECK(info->pid == 0);
        CHECK(info->restartCount == 0);
    }
    CHECK(!reg.get("B").has_value());
}

PP_TEST(full_lifecycle_and_pid_index) {
    ProcessRegistry reg;
    CHECK(reg.registerService("A").ok());
    CHECK(reg.transition("A", ProcessState::Starting).ok());
    CHECK(reg.markRunning("A", 4242, std::chrono::steady_clock::now()).ok());
    CHECK(reg.findByPid(4242).value_or("") == "A");
    CHECK(reg.get("A")->state == ProcessState::Running);

    CHECK(reg.transition("A", ProcessState::Failed).ok());
    reg.clearPid("A");
    CHECK(!reg.findByPid(4242).has_value());
    CHECK(reg.get("A")->pid == 0);

    CHECK(reg.transition("A", ProcessState::Restarting).ok());
    CHECK(reg.transition("A", ProcessState::Starting).ok());
    CHECK(reg.markRunning("A", 4243, std::chrono::steady_clock::now()).ok());
    CHECK(reg.incrementRestartCount("A").ok());
    CHECK(reg.get("A")->restartCount == 1);
    CHECK(reg.findByPid(4243).value_or("") == "A");
}

PP_TEST(illegal_transitions_are_rejected) {
    ProcessRegistry reg;
    CHECK(reg.registerService("A").ok());
    auto r = reg.transition("A", ProcessState::Running);  // Stopped -> Running is illegal
    CHECK(!r.ok());
    if (!r) CHECK(r.error().code == ErrorCode::InvalidState);
    CHECK(!reg.markRunning("A", 10, std::chrono::steady_clock::now()).ok());
    CHECK(reg.get("A")->state == ProcessState::Stopped);
    CHECK(!reg.transition("Nope", ProcessState::Starting).ok());
}

PP_TEST(transition_table) {
    CHECK(isValidTransition(ProcessState::Stopped, ProcessState::Starting));
    CHECK(isValidTransition(ProcessState::Starting, ProcessState::Running));
    CHECK(isValidTransition(ProcessState::Running, ProcessState::Stopping));
    CHECK(isValidTransition(ProcessState::Stopping, ProcessState::Stopped));
    CHECK(isValidTransition(ProcessState::Running, ProcessState::Failed));
    CHECK(isValidTransition(ProcessState::Failed, ProcessState::Restarting));
    CHECK(isValidTransition(ProcessState::Restarting, ProcessState::Starting));
    CHECK(isValidTransition(ProcessState::Failed, ProcessState::PermanentlyFailed));
    CHECK(!isValidTransition(ProcessState::Stopped, ProcessState::Running));
    CHECK(!isValidTransition(ProcessState::Running, ProcessState::Restarting));
    CHECK(!isValidTransition(ProcessState::PermanentlyFailed, ProcessState::Running));
    CHECK(toString(ProcessState::PermanentlyFailed) == "PERMANENTLY_FAILED");
}

PP_TEST(snapshot_keeps_registration_order) {
    ProcessRegistry reg;
    CHECK(reg.registerService("Z").ok());
    CHECK(reg.registerService("A").ok());
    CHECK(reg.registerService("M").ok());
    const auto snap = reg.snapshot();
    CHECK(snap.size() == 3u);
    CHECK(snap[0].serviceName == "Z" && snap[1].serviceName == "A" && snap[2].serviceName == "M");
}

PP_TEST_MAIN()
