#include "linux/signal_pipe.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <mutex>

#include "linux/file_descriptor.hpp"

namespace pilot {

namespace {

// The only global state in the project. The handler can only reach globals;
// std::atomic<int> is lock-free, hence safe to read from a signal handler.
std::atomic<int> g_writeFd{-1};

std::mutex g_installMutex;  // guards the objects below (never touched by the handler)
FileDescriptor g_readEnd;
FileDescriptor g_writeEnd;
bool g_installed = false;

void handleSignal(int signo) {
    const int savedErrno = errno;  // handlers must not clobber errno
    const int fd = g_writeFd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const unsigned char byte = static_cast<unsigned char>(signo);
        const ssize_t w = ::write(fd, &byte, 1);  // async-signal-safe; a full pipe just drops the byte
        (void)w;
    }
    errno = savedErrno;
}

Result<void> setHandler(int signo) {
    struct sigaction sa {};
    sa.sa_handler = handleSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;  // do not wake us for stopped/continued children
    if (::sigaction(signo, &sa, nullptr) != 0) {
        return makeError(ErrorCode::InternalError, "sigaction failed: " + errnoMessage(errno), errno);
    }
    return Result<void>::success();
}

}  // namespace

Result<void> SignalPipe::install() {
    std::scoped_lock lock(g_installMutex);
    if (g_installed) {
        return Result<void>::success();
    }
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        return makeError(ErrorCode::InternalError, "pipe2 failed: " + errnoMessage(errno), errno);
    }
    g_readEnd.reset(fds[0]);
    g_writeEnd.reset(fds[1]);
    g_writeFd.store(g_writeEnd.get());

    for (const int signo : {SIGINT, SIGTERM, SIGCHLD}) {
        if (auto r = setHandler(signo); !r) {
            g_writeFd.store(-1);
            g_readEnd.reset();
            g_writeEnd.reset();
            return r;
        }
    }
    g_installed = true;
    return Result<void>::success();
}

void SignalPipe::uninstall() {
    std::scoped_lock lock(g_installMutex);
    if (!g_installed) {
        return;
    }
    for (const int signo : {SIGINT, SIGTERM, SIGCHLD}) {
        ::signal(signo, SIG_DFL);
    }
    g_writeFd.store(-1);
    g_readEnd.reset();
    g_writeEnd.reset();
    g_installed = false;
}

bool SignalPipe::installed() {
    std::scoped_lock lock(g_installMutex);
    return g_installed;
}

void SignalPipe::notify(unsigned char event) noexcept {
    const int fd = g_writeFd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        const ssize_t w = ::write(fd, &event, 1);
        (void)w;
    }
}

std::vector<unsigned char> SignalPipe::wait(std::chrono::milliseconds timeout) {
    std::vector<unsigned char> events;
    int fd = -1;
    {
        std::scoped_lock lock(g_installMutex);
        fd = g_readEnd.get();
    }
    if (fd < 0) {
        return events;
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
    if (rc <= 0) {
        return events;  // timeout or EINTR: the caller loops and re-checks its state anyway
    }
    unsigned char buffer[64];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            events.insert(events.end(), buffer, buffer + n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;  // EAGAIN (drained) or EOF
    }
    return events;
}

}  // namespace pilot
