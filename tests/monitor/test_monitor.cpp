#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "linux/proc_reader.hpp"
#include "linux/sysfs_reader.hpp"
#include "monitor/health_monitor.hpp"
#include "process/process_registry.hpp"
#include "test_framework.hpp"

using namespace pilot;

namespace {

// Real kernel format: pid (comm) state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt
// utime stime cutime cstime priority nice threads itrealvalue starttime vsize rss ...
constexpr const char* kStat =
    "1234 (my (weird) name) S 1 1234 1234 0 -1 4194560 100 0 0 0 150 50 0 0 20 0 3 0 987654 20480000 500 "
    "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n";

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() / ("pp-test-" + std::to_string(::getpid()) + "-" +
                                                         std::to_string(std::rand()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

ProcessMetrics metrics(char state, pid_t ppid, unsigned long rssKb) {
    ProcessMetrics m;
    m.pid = 100;
    m.state = state;
    m.parentPid = ppid;
    m.residentKb = rssKb;
    return m;
}

}  // namespace

PP_TEST(parse_stat_handles_parentheses_in_comm) {
    ProcessMetrics m;
    auto r = ProcReader::parseStat(kStat, m);
    CHECK_MSG(r.ok(), r.ok() ? "" : r.error().message);
    CHECK(m.pid == 1234);
    CHECK(m.name == "my (weird) name");
    CHECK(m.state == 'S');
    CHECK(m.parentPid == 1);
    CHECK(m.userTicks == 150ull && m.systemTicks == 50ull);
    CHECK(m.totalCpuTicks() == 200ull);
    CHECK(m.threads == 3);
    CHECK(m.startTimeTicks == 987654ull);
    CHECK(m.virtualKb == 20000ul);
    CHECK(m.residentKb > 0ul);
}

PP_TEST(parse_stat_rejects_malformed_input) {
    ProcessMetrics m;
    CHECK(!ProcReader::parseStat("", m).ok());
    CHECK(!ProcReader::parseStat("garbage without parens", m).ok());
    CHECK(!ProcReader::parseStat("12 (x) S 1 2 3", m).ok());                       // too few fields
    CHECK(!ProcReader::parseStat("abc (x) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23", m).ok());
    CHECK(!ProcReader::parseStat("12 (x) S x 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23", m).ok());
}

PP_TEST(parse_status_and_cmdline) {
    ProcessMetrics m;
    ProcReader::parseStatus("Name:\tfoo\nVmSize:\t  2048 kB\nVmRSS:\t   512 kB\nThreads:\t4\nGarbageLine\n", m);
    CHECK(m.virtualKb == 2048ul && m.residentKb == 512ul && m.threads == 4);
    CHECK(ProcReader::parseCmdline(std::string("/bin/x\0--flag\0value\0", 20)) == "/bin/x --flag value");
    CHECK(ProcReader::parseCmdline("").empty());
}

PP_TEST(reads_own_process_from_real_proc) {
    ProcReader reader;
    auto r = reader.readProcess(::getpid());
    CHECK_MSG(r.ok(), r.ok() ? "" : r.error().describe());
    if (!r) return;
    CHECK(r.value().pid == ::getpid());
    CHECK(r.value().parentPid == ::getppid());
    CHECK(r.value().residentKb > 0ul);
    CHECK(r.value().threads >= 1);
    CHECK(!r.value().commandLine.empty());
}

PP_TEST(missing_process_reports_not_found) {
    ProcReader reader;
    auto r = reader.readProcess(INT_MAX);
    CHECK(!r.ok());
    if (!r) CHECK(r.error().code == ErrorCode::ProcessNotFound);
    auto bad = reader.readProcess(0);
    CHECK(!bad.ok());
}

PP_TEST(fake_proc_tree_handles_vanishing_and_malformed_files) {
    TempDir dir;
    ProcReader reader(dir.path);

    // Complete tree: succeeds, status values override stat values.
    writeFile(dir.path / "77" / "stat", kStat);
    writeFile(dir.path / "77" / "status", "VmRSS:\t 300 kB\nThreads:\t9\n");
    writeFile(dir.path / "77" / "cmdline", std::string("/bin/x\0-v\0", 10));
    auto r = reader.readProcess(77);
    CHECK_MSG(r.ok(), r.ok() ? "" : r.error().describe());
    if (r) {
        CHECK(r.value().residentKb == 300ul && r.value().threads == 9);
        CHECK(r.value().commandLine == "/bin/x -v");
    }

    // stat exists but status vanished: the process died mid-read -> ProcessNotFound.
    writeFile(dir.path / "79" / "stat", kStat);
    auto gone = reader.readProcess(79);
    CHECK(!gone.ok());
    if (!gone) CHECK(gone.error().code == ErrorCode::ProcessNotFound);

    writeFile(dir.path / "78" / "stat", "this is not a stat file");
    auto bad = reader.readProcess(78);
    CHECK(!bad.ok());
    if (!bad) CHECK(bad.error().code == ErrorCode::ProcReadFailed);
}

PP_TEST(evaluate_health_rules) {
    HealthSettings s;
    s.maxRssKb = 1000;
    CHECK(!evaluateHealth("A", metrics('S', 50, 500), s, 50).has_value());
    CHECK(evaluateHealth("A", metrics('Z', 50, 0), s, 50)->kind == HealthIssueKind::ZombieProcess);
    CHECK(evaluateHealth("A", metrics('T', 50, 0), s, 50)->kind == HealthIssueKind::Suspended);
    CHECK(evaluateHealth("A", metrics('S', 1, 0), s, 50)->kind == HealthIssueKind::Reparented);
    CHECK(evaluateHealth("A", metrics('S', 50, 5000), s, 50)->kind == HealthIssueKind::MemoryLimitExceeded);
    s.maxRssKb = 0;  // unlimited
    CHECK(!evaluateHealth("A", metrics('S', 50, 999999), s, 50).has_value());
}

PP_TEST(health_monitor_inspect_computes_cpu_percent) {
    ProcessRegistry registry;
    HealthMonitor monitor(registry, ProcReader{}, [](const HealthIssue&) {});
    auto first = monitor.inspect(::getpid());
    CHECK(first.ok());
    if (first) CHECK(first.value().cpuPercent == 0.0);
    volatile unsigned long spin = 0;
    for (unsigned long i = 0; i < 50000000ul; ++i) spin = spin + i;  // burn a little CPU
    auto second = monitor.inspect(::getpid());
    CHECK(second.ok());
    if (second) CHECK(second.value().cpuPercent >= 0.0);
}

PP_TEST(health_monitor_starts_and_stops_promptly) {
    ProcessRegistry registry;
    HealthMonitor monitor(registry, ProcReader{}, [](const HealthIssue&) {});
    monitor.configure({{"A", HealthSettings{std::chrono::milliseconds(5000), 0}}});
    monitor.start();
    const auto begin = std::chrono::steady_clock::now();
    monitor.stop();  // must wake the condition variable immediately, not wait for the interval
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(2));
    monitor.stop();  // idempotent
}

PP_TEST(sysfs_cpu_list_parsing) {
    CHECK(SysfsReader::countCpuList("0-3").value_or(0) == 4u);
    CHECK(SysfsReader::countCpuList("0-3,6,8-9\n").value_or(0) == 7u);
    CHECK(SysfsReader::countCpuList("0").value_or(0) == 1u);
    CHECK(!SysfsReader::countCpuList("").has_value());
    CHECK(!SysfsReader::countCpuList("3-1").has_value());
    CHECK(!SysfsReader::countCpuList("a-b").has_value());
}

PP_TEST(sysfs_reader_on_fake_tree) {
    TempDir dir;
    writeFile(dir.path / "devices/system/cpu/online", "0-3\n");
    writeFile(dir.path / "devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "2400000\n");
    writeFile(dir.path / "class/thermal/thermal_zone0/type", "x86_pkg_temp\n");
    writeFile(dir.path / "class/thermal/thermal_zone0/temp", "45500\n");
    writeFile(dir.path / "class/thermal/cooling_device0/type", "Processor\n");
    writeFile(dir.path / "class/net/lo/address", "00:00\n");

    SysfsReader reader(dir.path);
    auto snap = reader.snapshot();
    CHECK_MSG(snap.ok(), snap.ok() ? "" : snap.error().describe());
    if (!snap) return;
    CHECK(snap.value().onlineCpuCount == 4u);
    CHECK(snap.value().cpuFrequencyKhz.value_or(0) == 2400000ul);
    CHECK(snap.value().thermalZones.size() == 1u);
    if (!snap.value().thermalZones.empty()) {
        CHECK(snap.value().thermalZones[0].type == "x86_pkg_temp");
        CHECK(snap.value().thermalZones[0].celsius > 45.4 && snap.value().thermalZones[0].celsius < 45.6);
    }
    CHECK(snap.value().networkInterfaces.size() == 1u && snap.value().networkInterfaces[0] == "lo");
}

PP_TEST(sysfs_reader_degrades_gracefully) {
    TempDir dir;  // empty tree
    SysfsReader reader(dir.path);
    CHECK(!reader.snapshot().ok());
    CHECK(reader.listClass("net").empty());
    CHECK(!reader.readAttribute("../etc/passwd").ok());  // traversal rejected
    CHECK(!reader.readAttribute("devices/nothing").ok());
}

PP_TEST_MAIN()
