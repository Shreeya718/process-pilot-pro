// A tiny, controllable "service" used by config/*.conf and the tests.
//
//   demo-service run <name> <interval_ms>   heartbeat until SIGTERM/SIGINT, then exit 0 gracefully
//   demo-service sleep <ms>                 sleep (default signal handling: SIGTERM kills it)
//   demo-service crash <ms> <exit_code>     sleep, then exit with <exit_code>
//   demo-service exit <exit_code>           exit immediately
//   demo-service stubborn <ms>              ignores SIGTERM (needs SIGKILL)

#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

void sleepMs(long ms, bool interruptible) {
    while (ms > 0 && (!interruptible || g_stop == 0)) {
        const long slice = ms > 50 ? 50 : ms;
        timespec ts{};
        ts.tv_sec = slice / 1000;
        ts.tv_nsec = (slice % 1000) * 1000000L;
        nanosleep(&ts, nullptr);
        ms -= slice;
    }
}

long toLong(const char* text, long fallback) {
    if (text == nullptr) return fallback;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    return (end != text && *end == '\0') ? v : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";

    if (mode == "run") {
        const std::string name = argc > 2 ? argv[2] : "demo";
        const long interval = toLong(argc > 3 ? argv[3] : nullptr, 1000);
        struct sigaction sa {};
        sa.sa_handler = onSignal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        std::printf("[%s] started, pid=%d\n", name.c_str(), static_cast<int>(getpid()));
        std::fflush(stdout);
        unsigned long beat = 0;
        while (g_stop == 0) {
            sleepMs(interval, true);
            if (g_stop == 0) {
                std::printf("[%s] heartbeat %lu\n", name.c_str(), ++beat);
                std::fflush(stdout);
            }
        }
        std::printf("[%s] graceful shutdown\n", name.c_str());
        std::fflush(stdout);
        return 0;
    }
    if (mode == "sleep") {
        sleepMs(toLong(argc > 2 ? argv[2] : nullptr, 1000), false);
        return 0;
    }
    if (mode == "crash") {
        sleepMs(toLong(argc > 2 ? argv[2] : nullptr, 1000), false);
        return static_cast<int>(toLong(argc > 3 ? argv[3] : nullptr, 1));
    }
    if (mode == "exit") {
        return static_cast<int>(toLong(argc > 2 ? argv[2] : nullptr, 0));
    }
    if (mode == "stubborn") {
        std::signal(SIGTERM, SIG_IGN);
        sleepMs(toLong(argc > 2 ? argv[2] : nullptr, 60000), false);
        return 0;
    }

    std::fprintf(stderr, "usage: demo-service run <name> <ms> | sleep <ms> | crash <ms> <code> | exit <code> | stubborn <ms>\n");
    return 2;
}
