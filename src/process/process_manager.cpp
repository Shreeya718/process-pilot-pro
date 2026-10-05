#include "process/process_manager.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <thread>

#include "linux/file_descriptor.hpp"

namespace pilot {

namespace {

ProcessExitStatus decode(pid_t pid, int status) {
    ProcessExitStatus s;
    s.pid = pid;
    if (WIFEXITED(status)) {
        s.exited = true;
        s.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        s.signaled = true;
        s.termSignal = WTERMSIG(status);
        s.coreDumped = WCOREDUMP(status) != 0;
    }
    return s;
}

/// Runs in the forked child. Only async-signal-safe calls are allowed here
/// (the parent is multi-threaded): no malloc, no iostream, no logging.
[[noreturn]] void runChild(int errFd, const char* workDir, char* const* argv) {
    ::setpgid(0, 0);  // own process group: terminal Ctrl-C does not hit children directly

    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, nullptr);  // do not inherit a blocked mask

    const int devNull = ::open("/dev/null", O_RDONLY);
    if (devNull >= 0) {
        ::dup2(devNull, STDIN_FILENO);
        if (devNull > STDERR_FILENO) {
            ::close(devNull);
        }
    }

    if (workDir != nullptr && ::chdir(workDir) != 0) {
        const int e = errno;
        const ssize_t w = ::write(errFd, &e, sizeof(e));
        (void)w;
        ::_exit(127);
    }

    ::execvp(argv[0], argv);

    // execvp only returns on failure.
    const int e = errno;
    const ssize_t w = ::write(errFd, &e, sizeof(e));
    (void)w;
    ::_exit(127);
}

}  // namespace

std::string ProcessExitStatus::describe() const {
    if (exited) {
        return "exited with code " + std::to_string(exitCode);
    }
    if (signaled) {
        const char* name = ::strsignal(termSignal);
        return "killed by signal " + std::to_string(termSignal) + " (" + (name != nullptr ? name : "?") + ")" +
               (coreDumped ? " [core dumped]" : "");
    }
    return "exit status unavailable";
}

Result<pid_t> ProcessManager::start(const ServiceDefinition& service) {
    if (service.executable.empty()) {
        return makeError(ErrorCode::InvalidConfiguration, "service '" + service.name + "' has no executable");
    }

    // 1. Everything that allocates happens before fork().
    std::vector<std::string> argStore;
    argStore.reserve(service.arguments.size() + 1);
    argStore.push_back(service.executable);
    argStore.insert(argStore.end(), service.arguments.begin(), service.arguments.end());
    std::vector<char*> argv;
    argv.reserve(argStore.size() + 1);
    for (auto& a : argStore) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    const char* workDir = service.workingDirectory.empty() ? nullptr : service.workingDirectory.c_str();

    // 2. Close-on-exec pipe: closes automatically when exec succeeds.
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        return makeError(ErrorCode::InternalError, "pipe2 failed: " + errnoMessage(errno), errno);
    }
    FileDescriptor readEnd(fds[0]);
    FileDescriptor writeEnd(fds[1]);

    // 3. fork()
    const pid_t pid = ::fork();
    if (pid < 0) {
        const int e = errno;
        return makeError(ErrorCode::ForkFailed,
                         "fork failed for '" + service.name + "': " + errnoMessage(e), e);
    }
    if (pid == 0) {
        runChild(writeEnd.get(), workDir, argv.data());  // never returns
    }

    // 4. Parent
    writeEnd.reset();
    ::setpgid(pid, pid);  // also done in the child; whichever runs first wins (EACCES after exec is harmless)

    int childErrno = 0;
    ssize_t n;
    do {
        n = ::read(readEnd.get(), &childErrno, sizeof(childErrno));
    } while (n < 0 && errno == EINTR);

    if (n == 0) {
        return pid;  // pipe closed by exec: the new program is running
    }

    // Exec failed (or the pipe misbehaved): make sure the child is gone and reaped.
    const int readErrno = errno;
    if (n < 0 || n != static_cast<ssize_t>(sizeof(childErrno))) {
        ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return makeError(ErrorCode::InternalError,
                         "could not confirm exec of '" + service.executable + "': " +
                             (n < 0 ? errnoMessage(readErrno) : std::string("short read from error pipe")));
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    const ErrorCode code = (childErrno == EACCES || childErrno == EPERM) ? ErrorCode::PermissionDenied
                                                                         : ErrorCode::ExecFailed;
    return makeError(code,
                     "cannot execute '" + service.executable + "' for service '" + service.name +
                         "': " + errnoMessage(childErrno),
                     childErrno);
}

Result<void> ProcessManager::sendSignal(pid_t pid, int signo) {
    if (pid <= 0) {
        return makeError(ErrorCode::InvalidState, "refusing to signal invalid pid " + std::to_string(pid));
    }
    // Children lead their own process group (pgid == pid): signal the whole group first.
    if (::kill(-pid, signo) == 0) {
        return Result<void>::success();
    }
    if (::kill(pid, signo) == 0) {
        return Result<void>::success();
    }
    const int e = errno;
    if (e == ESRCH) {
        return makeError(ErrorCode::ProcessNotFound, "process " + std::to_string(pid) + " does not exist", e);
    }
    if (e == EPERM) {
        return makeError(ErrorCode::PermissionDenied,
                         "not permitted to signal process " + std::to_string(pid), e);
    }
    return makeError(ErrorCode::InternalError, "kill failed: " + errnoMessage(e), e);
}

Result<ProcessExitStatus> ProcessManager::wait(pid_t pid) {
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        if (errno == ECHILD) {
            ProcessExitStatus unknown;
            unknown.pid = pid;
            return unknown;  // already reaped elsewhere
        }
        return makeError(ErrorCode::InternalError, "waitpid failed: " + errnoMessage(errno), errno);
    }
    return decode(pid, status);
}

Result<std::optional<ProcessExitStatus>> ProcessManager::tryWait(pid_t pid) {
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(pid, &status, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        if (errno == ECHILD) {
            ProcessExitStatus unknown;
            unknown.pid = pid;
            return std::optional<ProcessExitStatus>(unknown);
        }
        return makeError(ErrorCode::InternalError, "waitpid failed: " + errnoMessage(errno), errno);
    }
    if (r == 0) {
        return std::optional<ProcessExitStatus>(std::nullopt);
    }
    return std::optional<ProcessExitStatus>(decode(pid, status));
}

std::vector<ProcessExitStatus> ProcessManager::reapAll() {
    std::vector<ProcessExitStatus> out;
    for (;;) {
        int status = 0;
        const pid_t r = ::waitpid(-1, &status, WNOHANG);
        if (r > 0) {
            out.push_back(decode(r, status));
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            break;  // 0 = children exist but none finished, <0 = ECHILD (no children)
        }
    }
    return out;
}

Result<ProcessExitStatus> ProcessManager::stop(pid_t pid, std::chrono::milliseconds timeout) {
    if (pid <= 0) {
        return makeError(ErrorCode::InvalidState, "cannot stop invalid pid " + std::to_string(pid));
    }

    auto early = tryWait(pid);
    if (!early) return early.error();
    if (early.value()) return *early.value();

    if (auto r = sendSignal(pid, SIGTERM); !r && r.error().code != ErrorCode::ProcessNotFound) {
        return r.error();
    }
    if (auto r = sendSignal(pid, SIGCONT); !r && r.error().code != ErrorCode::ProcessNotFound) {
        return r.error();  // SIGCONT wakes a SIGSTOPped child so it can see SIGTERM
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto st = tryWait(pid);
        if (!st) return st.error();
        if (st.value()) return *st.value();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (auto r = sendSignal(pid, SIGKILL); !r && r.error().code != ErrorCode::ProcessNotFound) {
        return r.error();
    }
    return wait(pid);
}

Result<pid_t> ProcessManager::restart(const ServiceDefinition& service, pid_t oldPid,
                                      std::chrono::milliseconds timeout) {
    if (oldPid > 0) {
        auto stopped = stop(oldPid, timeout);
        if (!stopped) return stopped.error();
    }
    return start(service);
}

}  // namespace pilot
