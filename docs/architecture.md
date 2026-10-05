# Architecture

## Components and data flow
`ConfigParser` → `ServiceDefinition`s → `DependencyGraph` (validation + order) → `ProcessSupervisor`, which owns the
`ProcessManager`, `ProcessRegistry`, per-service `RestartPolicy`, `HealthMonitor` and `ControlServer`.

## Threads
| Thread | Purpose | Blocks on |
|---|---|---|
| main | supervision loop: signals, reaping, due restarts, shutdown | `poll()` on the signal pipe (timeout = next restart or 1 s) |
| health | `/proc` inspection of RUNNING services | `condition_variable::wait_for` |
| control | CLI commands over a Unix socket | `poll()` on {listen socket, wake pipe} |

No thread busy-waits. The only polling sleep is the 10 ms `waitpid(WNOHANG)` loop inside a bounded graceful stop.

## Synchronisation (who owns what)
| State | Owner / protection |
|---|---|
| lifecycle operations, `policies_`, `pendingRestarts_` | `ProcessSupervisor::mutex_` (operation lock) |
| service → ProcessInfo, PID → service | `ProcessRegistry::mutex_` (leaf lock; getters return copies) |
| stop flag, health settings | `HealthMonitor::mutex_` + condition variable |
| CPU samples | `HealthMonitor::sampleMutex_` |
| log output, log file | `Logger::mutex_` |
| signal pipe fd | `std::atomic<int>` (read by the handler) - the only global |
| service definitions, orders | written once in `loadDefinitions()`, immutable afterwards |

**Lock order:** `ProcessSupervisor::mutex_` → `ProcessRegistry::mutex_`. The registry never calls out while locked, and
the health monitor calls its callback with no monitor lock held, so no cycle exists. `HealthMonitor::stop()` and
`ControlServer::stop()` join threads and are therefore always called **without** holding `mutex_`.

## Error handling
`Result<T>` / `Result<void>` (`[[nodiscard]]`) carry an `Error{ErrorCode, message, errno}`; every system call
that can fail (`fork`, `execvp`, `waitpid`, `kill`, `open`, `read`, `pipe2`, `socket`, ...) is checked.

## Memory / resource management
RAII throughout: `FileDescriptor` owns descriptors (all opened `O_CLOEXEC`), `std::unique_ptr` owns monitor/control
objects, standard containers own data. There is no `new`/`delete`.
