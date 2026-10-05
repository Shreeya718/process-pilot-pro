#include "config/config_parser.hpp"
#include "test_framework.hpp"

using namespace pilot;

namespace {
constexpr const char* kValid = R"(
# demo
[Logger]
executable = /bin/logger
args = --level debug "two words"

[Database]
executable = /bin/db
dependencies = [Logger]
restart_policy = always
max_restarts = 3
restart_delay_ms = 250
restart_window_s = 30
health_interval_ms = 500
max_rss_kb = 2048
stop_timeout_ms = 1500
working_dir = /tmp

[WebApp]
executable = /bin/web
dependencies = Database, Logger
enabled = false
health_check = none
)";
}  // namespace

PP_TEST(parses_valid_configuration) {
    auto r = ConfigParser{}.parseString(kValid);
    CHECK_MSG(r.ok(), r.ok() ? "" : r.error().message);
    if (!r) return;
    const auto& s = r.value();
    CHECK(s.size() == 3u);
    CHECK(s[0].name == "Logger");
    CHECK(s[0].arguments.size() == 3u);
    CHECK(s[0].arguments[2] == "two words");
    CHECK(s[1].dependencies.size() == 1u && s[1].dependencies[0] == "Logger");
    CHECK(s[1].restart.mode == RestartMode::Always);
    CHECK(s[1].restart.maxRestarts == 3u);
    CHECK(s[1].restart.delay.count() == 250);
    CHECK(s[1].restart.window.count() == 30);
    CHECK(s[1].health.interval.count() == 500);
    CHECK(s[1].health.maxRssKb == 2048ul);
    CHECK(s[1].stopTimeout.count() == 1500);
    CHECK(s[1].workingDirectory == "/tmp");
    CHECK(!s[2].enabled);
    CHECK(s[2].health.mode == HealthCheckMode::None);
    CHECK(s[2].dependencies.size() == 2u);
}

PP_TEST(defaults_are_applied) {
    auto r = ConfigParser{}.parseString("[A]\nexecutable = /bin/true\n");
    CHECK(r.ok());
    if (!r) return;
    CHECK(r.value()[0].restart.mode == RestartMode::OnFailure);
    CHECK(r.value()[0].restart.maxRestarts == 5u);
    CHECK(r.value()[0].enabled);
}

PP_TEST(rejects_missing_executable) {
    auto r = ConfigParser{}.parseString("[A]\nrestart_policy = never\n");
    CHECK(!r.ok());
    if (!r) CHECK(r.error().code == ErrorCode::InvalidConfiguration);
}

PP_TEST(rejects_unknown_key_with_line_number) {
    auto r = ConfigParser{}.parseString("[A]\nexecutable = /bin/true\nbogus = 1\n", "test.conf");
    CHECK(!r.ok());
    if (!r) {
        CHECK(r.error().message.find("test.conf:3") != std::string::npos);
        CHECK(r.error().message.find("bogus") != std::string::npos);
    }
}

PP_TEST(rejects_bad_values) {
    CHECK(!ConfigParser{}.parseString("[A]\nexecutable=/x\nrestart_policy = sometimes\n").ok());
    CHECK(!ConfigParser{}.parseString("[A]\nexecutable=/x\nmax_restarts = -1\n").ok());
    CHECK(!ConfigParser{}.parseString("[A]\nexecutable=/x\nmax_restarts = ten\n").ok());
    CHECK(!ConfigParser{}.parseString("[A]\nexecutable=/x\nenabled = maybe\n").ok());
    CHECK(!ConfigParser{}.parseString("[A]\nexecutable=/x\nargs = \"unterminated\n").ok());
    CHECK(!ConfigParser{}.parseString("[A]\nexecutable=/x\nhealth_interval_ms = 10\n").ok());
}

PP_TEST(rejects_structural_errors) {
    CHECK(!ConfigParser{}.parseString("executable = /x\n").ok());              // outside section
    CHECK(!ConfigParser{}.parseString("[A\nexecutable = /x\n").ok());          // bad header
    CHECK(!ConfigParser{}.parseString("[A]\nnot a pair\n").ok());              // no '='
    CHECK(!ConfigParser{}.parseString("[A]\nexecutable=/x\n[A]\nexecutable=/y\n").ok());  // duplicate
    CHECK(!ConfigParser{}.parseString("# only comments\n").ok());              // no services
    CHECK(!ConfigParser{}.parseString("[bad name]\nexecutable=/x\n").ok());    // invalid name
}

PP_TEST(rejects_enabled_service_depending_on_disabled_one) {
    auto r = ConfigParser{}.parseString(
        "[A]\nexecutable=/x\nenabled=false\n[B]\nexecutable=/y\ndependencies=A\n");
    CHECK(!r.ok());
}

PP_TEST(parseFile_reports_missing_file) {
    auto r = ConfigParser{}.parseFile("/definitely/not/here.conf");
    CHECK(!r.ok());
    if (!r) CHECK(r.error().code == ErrorCode::InvalidConfiguration);
}

PP_TEST(split_arguments_handles_quotes) {
    auto r = ConfigParser::splitArguments("a 'b c' \"d e\" f\"g h\"i  ");
    CHECK(r.ok());
    if (!r) return;
    CHECK(r.value().size() == 4u);
    CHECK(r.value()[1] == "b c");
    CHECK(r.value()[3] == "fg hi");
    auto empty = ConfigParser::splitArguments("   ");
    CHECK(empty.ok() && empty.value().empty());
}

PP_TEST_MAIN()
