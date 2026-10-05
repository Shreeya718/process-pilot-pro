#pragma once

#include <sys/types.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "common/result.hpp"
#include "config/service_config.hpp"

namespace pilot {

/// How a child terminated (decoded waitpid() status).
struct ProcessExitStatus {
    pid_t pid{0};
    bool exited{false};      // terminated normally via exit()
    int exitCode{0};
    bool signaled{false};    // terminated by a signal
    int termSignal{0};
    bool coreDumped{false};

    /// Non-zero exit code or killed by a signal.
    [[nodiscard]] bool abnormal() const noexcept { return signaled || (exited && exitCode != 0); }
    [[nodiscard]] std::string describe() const;
};

/// Stateless wrapper around fork/exec/waitpid/kill for ONE child at a time.
/// PID bookkeeping lives in ProcessRegistry; configuration parsing is not its job.
///
/// Launch protocol (start()):
///   1. build argv *before* fork() (no allocation in the child),
///   2. create a close-on-exec "error pipe",
///   3. fork():  child -> setpgid, reset signal mask, stdin=/dev/null, execvp();
///               on exec failure the child writes errno into the pipe and _exit(127),
///   4. parent closes its write end and reads the pipe:
///        EOF            -> exec succeeded (the pipe was closed by the kernel at exec),
///        sizeof(errno)  -> exec failed; the child is reaped and ExecFailed returned.
/// This way a PID is never trusted merely because fork() returned.
class ProcessManager {
public:
    [[nodiscard]] Result<pid_t> start(const ServiceDefinition& service);

    /// SIGTERM (+SIGCONT) to the child's process group, wait up to `timeout`,
    /// then SIGKILL. Always reaps the child before returning.
    [[nodiscard]] Result<ProcessExitStatus> stop(pid_t pid, std::chrono::milliseconds timeout);

    /// stop(oldPid) then start().
    [[nodiscard]] Result<pid_t> restart(const ServiceDefinition& service, pid_t oldPid,
                                        std::chrono::milliseconds timeout);

    /// Blocking waitpid(pid).
    [[nodiscard]] Result<ProcessExitStatus> wait(pid_t pid);

    /// Non-blocking waitpid(pid, WNOHANG): nullopt while the child is still running.
    [[nodiscard]] Result<std::optional<ProcessExitStatus>> tryWait(pid_t pid);

    /// Reap every terminated child (waitpid(-1, WNOHANG) loop) -> no zombies.
    [[nodiscard]] std::vector<ProcessExitStatus> reapAll();

    [[nodiscard]] Result<void> sendSignal(pid_t pid, int signo);
};

}  // namespace pilot
