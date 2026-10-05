// End-to-end tests: the real supervisor forks/execs the real `demo-service` binary.

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <functional>
#include <thread>

#include "logging/logger.hpp"
#include "supervisor/control_server.hpp"
#include "supervisor/supervisor.hpp"
#include "test_framework.hpp"

using namespace pilot;
using namespace std::chrono_literals;

#ifndef DEMO_SERVICE_PATH
#error "DEMO_SERVICE_PATH must be defined by CMake"
#endif

namespace {

ServiceDefinition service(const std::string& name, std::vector<std::string> args,
                          std::vector<std::string> deps = {}) {
    ServiceDefinition s;
    s.name = name;
    s.executable = DEMO_SERVICE_PATH;
    s.arguments = std::move(args);
    s.dependencies = std::move(deps);
    s.restart.mode = RestartMode::OnFailure;
    s.restart.delay = 50ms;
    s.restart.window = 0s;
    s.health.interval = 100ms;
    s.stopTimeout = 2000ms;
    return s;
}

bool waitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return predicate();
}

bool processExists(pid_t pid) { return ::kill(pid, 0) == 0 || errno != ESRCH; }

ProcessInfo find(const ProcessSupervisor& sup, const std::string& name) {
    for (const auto& p : sup.status()) {
        if (p.serviceName == name) return p;
    }
    return {};
}

/// Runs supervisor.run() on a background thread (as the CLI's main thread would).
struct RunningSupervisor {
    ProcessSupervisor& sup;
    std::thread thread;
    bool runOk{false};
    explicit RunningSupervisor(ProcessSupervisor& s) : sup(s) {
        thread = std::thread([this] { runOk = sup.run().ok(); });
    }
    void shutdown() {
        sup.requestShutdown();
        if (thread.joinable()) thread.join();
    }
    ~RunningSupervisor() { shutdown(); }
};

}  // namespace

PP_TEST(starts_in_dependency_order_and_stops_in_reverse) {
    Logger::instance().setLevel(LogLevel::Warning);
    ProcessSupervisor sup;
    auto loaded = sup.loadDefinitions({service("WebApp", {"sleep", "60000"}, {"Database"}),
                                       service("Database", {"sleep", "60000"}, {"Logger"}),
                                       service("Logger", {"sleep", "60000"})});
    CHECK_MSG(loaded.ok(), loaded.ok() ? "" : loaded.error().message);
    if (!loaded) return;
    CHECK((sup.startupOrder() == std::vector<std::string>{"Logger", "Database", "WebApp"}));
    CHECK((sup.shutdownOrder() == std::vector<std::string>{"WebApp", "Database", "Logger"}));

    auto started = sup.startAll();
    CHECK_MSG(started.ok(), started.ok() ? "" : started.error().message);
    if (!started) return;

    std::vector<pid_t> pids;
    for (const auto& p : sup.status()) {
        CHECK(p.state == ProcessState::Running);
        CHECK(p.pid > 0);
        CHECK(processExists(p.pid));
        pids.push_back(p.pid);
    }
    CHECK(pids.size() == 3u && pids[0] != pids[1] && pids[1] != pids[2]);

    CHECK(sup.stopAll().ok());
    for (const auto& p : sup.status()) CHECK(p.state == ProcessState::Stopped);
    for (const pid_t pid : pids) CHECK(!processExists(pid));  // reaped: no zombies left behind
}

PP_TEST(exec_failure_is_reported_and_rolled_back) {
    Logger::instance().setLevel(LogLevel::Critical);
    ProcessSupervisor sup;
    ServiceDefinition ok = service("Good", {"sleep", "60000"});
    ServiceDefinition bad = service("Bad", {}, {"Good"});
    bad.executable = "/definitely/not/a/binary";
    CHECK(sup.loadDefinitions({ok, bad}).ok());

    auto r = sup.startAll();
    CHECK(!r.ok());
    if (!r) CHECK(r.error().code == ErrorCode::ExecFailed);
    CHECK(find(sup, "Bad").state == ProcessState::Failed);
    CHECK(find(sup, "Good").state == ProcessState::Stopped);  // rolled back
    CHECK(find(sup, "Good").pid == 0);
}

PP_TEST(rejects_circular_configuration) {
    ProcessSupervisor sup;
    auto r = sup.loadDefinitions({service("A", {}, {"B"}), service("B", {}, {"A"})});
    CHECK(!r.ok());
    if (!r) CHECK(r.error().code == ErrorCode::DependencyCycle);
}

PP_TEST(crashing_service_is_restarted_then_permanently_failed) {
    Logger::instance().setLevel(LogLevel::Critical);
    ProcessSupervisor sup;
    ServiceDefinition crasher = service("Crasher", {"crash", "100", "3"});
    crasher.restart.maxRestarts = 2;
    CHECK(sup.loadDefinitions({crasher}).ok());
    CHECK(sup.startAll().ok());

    RunningSupervisor loop(sup);
    const bool failed = waitFor([&] { return find(sup, "Crasher").state == ProcessState::PermanentlyFailed; }, 15s);
    CHECK_MSG(failed, "service never reached PERMANENTLY_FAILED");
    CHECK(find(sup, "Crasher").restartCount == 2);
    CHECK(find(sup, "Crasher").pid == 0);
    loop.shutdown();
    CHECK(loop.runOk);
}

PP_TEST(clean_exit_is_not_restarted_under_on_failure) {
    Logger::instance().setLevel(LogLevel::Critical);
    ProcessSupervisor sup;
    CHECK(sup.loadDefinitions({service("OneShot", {"exit", "0"})}).ok());
    CHECK(sup.startAll().ok());
    RunningSupervisor loop(sup);
    CHECK(waitFor([&] { return find(sup, "OneShot").state == ProcessState::Stopped; }, 5s));
    std::this_thread::sleep_for(300ms);
    CHECK(find(sup, "OneShot").state == ProcessState::Stopped);
    CHECK(find(sup, "OneShot").restartCount == 0);
}

PP_TEST(killed_service_is_restarted_with_new_pid) {
    Logger::instance().setLevel(LogLevel::Critical);
    ProcessSupervisor sup;
    CHECK(sup.loadDefinitions({service("Victim", {"sleep", "60000"})}).ok());
    CHECK(sup.startAll().ok());
    RunningSupervisor loop(sup);

    const pid_t first = find(sup, "Victim").pid;
    CHECK(first > 0);
    ::kill(first, SIGKILL);  // simulate a crash from the outside
    const bool restarted = waitFor(
        [&] {
            const auto p = find(sup, "Victim");
            return p.state == ProcessState::Running && p.pid > 0 && p.pid != first;
        },
        10s);
    CHECK_MSG(restarted, "service was not restarted with a new PID");
    CHECK(find(sup, "Victim").restartCount == 1);
}

PP_TEST(stubborn_service_is_killed_after_timeout) {
    Logger::instance().setLevel(LogLevel::Critical);
    ProcessSupervisor sup;
    ServiceDefinition s = service("Stubborn", {"stubborn", "60000"});
    s.stopTimeout = 300ms;
    CHECK(sup.loadDefinitions({s}).ok());
    CHECK(sup.startAll().ok());
    const pid_t pid = find(sup, "Stubborn").pid;
    std::this_thread::sleep_for(500ms);  // let the child install SIG_IGN

    const auto begin = std::chrono::steady_clock::now();
    CHECK(sup.stopAll().ok());
    const auto took = std::chrono::steady_clock::now() - begin;
    CHECK(took >= 250ms);                 // SIGTERM was ignored, escalation happened
    CHECK(took < 5s);
    CHECK(!processExists(pid));
    CHECK(find(sup, "Stubborn").state == ProcessState::Stopped);
}

PP_TEST(memory_limit_violation_triggers_restart_cycle) {
    Logger::instance().setLevel(LogLevel::Critical);
    ProcessSupervisor sup;
    ServiceDefinition hog = service("Hog", {"sleep", "60000"});
    hog.health.maxRssKb = 1;  // any real process exceeds 1 kB
    hog.restart.maxRestarts = 1;
    CHECK(sup.loadDefinitions({hog}).ok());
    CHECK(sup.startAll().ok());
    RunningSupervisor loop(sup);
    const bool failed = waitFor([&] { return find(sup, "Hog").state == ProcessState::PermanentlyFailed; }, 20s);
    CHECK_MSG(failed, "health monitor did not drive the service to PERMANENTLY_FAILED");
    CHECK(find(sup, "Hog").restartCount == 1);
}

PP_TEST(control_socket_status_restart_and_stop) {
    Logger::instance().setLevel(LogLevel::Critical);
    const std::filesystem::path socket = "/tmp/pp-test-" + std::to_string(::getpid()) + ".sock";
    SupervisorOptions options;
    options.controlSocketPath = socket;
    ProcessSupervisor sup(options);
    CHECK(sup.loadDefinitions({service("Logger", {"sleep", "60000"}),
                               service("App", {"sleep", "60000"}, {"Logger"})}).ok());
    CHECK(sup.startAll().ok());

    RunningSupervisor loop(sup);
    CHECK(waitFor([&] { return sendControlCommand(socket, "ping", 500ms).ok(); }, 5s));

    auto status = sendControlCommand(socket, "status");
    CHECK(status.ok());
    if (status) {
        CHECK(status.value().find("SERVICE") != std::string::npos);
        CHECK(status.value().find("RUNNING") != std::string::npos);
        CHECK(status.value().find("Logger") != std::string::npos);
    }

    const pid_t before = find(sup, "App").pid;
    auto restarted = sendControlCommand(socket, "restart App", 30s);
    CHECK(restarted.ok() && restarted.value().rfind("OK", 0) == 0);
    CHECK(find(sup, "App").pid != before);
    CHECK(find(sup, "App").restartCount == 1);

    auto unknown = sendControlCommand(socket, "restart Nope", 5s);
    CHECK(unknown.ok() && unknown.value().rfind("ERROR", 0) == 0);

    auto monitor = sendControlCommand(socket, "monitor");
    CHECK(monitor.ok() && monitor.value().find("RSS(MB)") != std::string::npos);

    auto stop = sendControlCommand(socket, "stop");
    CHECK(stop.ok());
    loop.shutdown();
    CHECK(loop.runOk);
    CHECK(find(sup, "App").state == ProcessState::Stopped);
    CHECK(!std::filesystem::exists(socket));  // socket file is cleaned up
}

PP_TEST_MAIN()
