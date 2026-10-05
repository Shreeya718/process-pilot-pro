# Process lifecycle

```
STOPPED → STARTING → RUNNING → STOPPING → STOPPED
RUNNING → FAILED → RESTARTING → STARTING → RUNNING
FAILED  → PERMANENTLY_FAILED
```

## Launch (`ProcessManager::start`)
1. argv is built before `fork()` (no allocation in the child).
2. A `pipe2(O_CLOEXEC)` "error pipe" is created.
3. `fork()` - failure → `ErrorCode::ForkFailed`.
4. Child: `setpgid`, clear signal mask, stdin ← `/dev/null`, optional `chdir`, `execvp`. If exec fails the child writes
   `errno` to the pipe and `_exit(127)` (only async-signal-safe calls run in the child).
5. Parent: closes the write end, `read()`s the pipe. EOF = the kernel closed it at `exec` → success. A full `int` =
   exec failed → child is reaped, `ExecFailed` / `PermissionDenied` returned. A PID is never trusted just because
   `fork()` returned.

## Exit handling
`waitpid(-1, WNOHANG)` loop on every wake-up (no zombies). Exit code 0 = clean, non-zero or signal = abnormal.

| policy | clean exit | abnormal exit |
|---|---|---|
| never | STOPPED | FAILED |
| on_failure | STOPPED | restart (until limit) |
| always | restart (until limit) | restart (until limit) |

## Crash-loop protection
Each counted failure is timestamped. Failures older than `restart_window_s` are discarded. When more than
`max_restarts` remain → `PERMANENTLY_FAILED` (window 0 = count over the whole lifetime). Restarts wait `restart_delay_ms`
without blocking the loop (the supervisor computes the next due time and sleeps in `poll()` until then).

## Stopping
`SIGTERM` (+`SIGCONT`) to the process group, wait up to `stop_timeout_ms`, then `SIGKILL`, then blocking `waitpid`.
Shutdown stops services in reverse topological order.
