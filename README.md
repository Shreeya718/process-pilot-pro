# Process Pilot Pro

**Linux-Based Process Supervisor with Dependency Resolution, Health Monitoring and Self-Healing**

A lightweight supervisor (in the spirit of `systemd`/`supervisord`) written in **pure C++20** for Linux.
It reads service definitions, orders them with a topological sort, launches them with `fork()` + `execvp()`,
watches them through `/proc`, and automatically restarts them according to a restart policy with crash-loop
protection. Capstone project (Wipro).

* Language: C++20 only (GCC or Clang) - CMake is used purely as build configuration.
* Platform: Linux (x86_64 recommended). No third-party libraries.

## Quick start

```bash
# Requirements: g++ >= 10 or clang++ >= 12, cmake >= 3.16, make (Ubuntu: sudo apt install build-essential cmake)
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure     # unit + integration tests

./build/process-pilot validate config/example.conf
./build/process-pilot start    config/example.conf      # terminal 1 (runs in the foreground)

./build/process-pilot status                            # terminal 2
./build/process-pilot monitor --count 3
./build/process-pilot restart WebApp
./build/process-pilot stop                              # or press Ctrl-C in terminal 1
```

Run the commands from the repository root (the example configs use the relative path `build/demo-service`).

Self-healing demo (a crashing service is restarted twice, then declared `PERMANENTLY_FAILED`):

```bash
./build/process-pilot start config/test.conf
```

Cycle detection demo: `./build/process-pilot validate config/cycle.conf` prints
`ERROR: Circular dependency detected: A -> B -> C -> A` and exits with status 1.

Build variants:

| Purpose | Command |
|---|---|
| Debug (default) | `cmake -S . -B build` |
| Release (`-O3`) | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` |
| ASan + UBSan | `cmake -S . -B build-asan -DPROCESS_PILOT_SANITIZERS=ON` |
| Without `-Werror` | `-DPROCESS_PILOT_WERROR=OFF` |
| Clang | `CXX=clang++ cmake -S . -B build-clang` |

All builds use `-std=c++20 -Wall -Wextra -Wpedantic -Werror`.

## CLI

```
process-pilot validate <config>     parse + validate, print startup / shutdown order
process-pilot list     <config>     table of services, dependencies, restart policy
process-pilot start    <config>     start everything and supervise (foreground)
process-pilot stop                  graceful shutdown of the running supervisor
process-pilot restart  [service]    restart one service (or all)
process-pilot status                SERVICE / PID / STATE / RESTARTS
process-pilot monitor               CPU%, RSS, threads, uptime from /proc
options: --socket PATH  --log-file PATH  --log-level LEVEL  --interval MS  --count N
```

```
SERVICE         PID      STATE               RESTARTS
Logger          516      RUNNING             0
Database        518      RUNNING             0
WebApp          519      RUNNING             1
Flaky           -        PERMANENTLY_FAILED  2
```

`stop`, `restart`, `status` and `monitor` talk to the running supervisor over a Unix-domain socket
(default `/tmp/process-pilot-<uid>.sock`, mode 0600) - this is the project's inter-process communication.

## Configuration format

INI-like, parsed by a hand-written C++20 parser (comments on their own line start with `#` or `;`):

```ini
[Database]
executable         = /usr/bin/mydb
args               = --port 5432 "--name=my db"
dependencies       = Logger                 # comma separated; "[Logger]" also accepted
restart_policy     = on_failure             # never | on_failure | always
max_restarts       = 5                      # failures tolerated inside the window
restart_delay_ms   = 1000
restart_window_s   = 60                     # crash-loop window (0 = whole lifetime)
health_check       = process                # process | none
health_interval_ms = 2000
max_rss_kb         = 0                      # 0 = unlimited
stop_timeout_ms    = 5000                   # SIGTERM -> SIGKILL escalation
working_dir        = /var/lib/mydb
enabled            = true
```

## Architecture

```
CLI ──► ProcessSupervisor ──► ConfigParser ► DependencyGraph (Kahn, cycle detection)
              │                  ProcessManager (fork/execvp/waitpid/kill)
              │                  ProcessRegistry (state machine, PID index)  ◄── HealthMonitor thread ◄── ProcReader (/proc)
              │                  RestartPolicy (never/on_failure/always + crash-loop window)
              │                  SignalPipe (SIGINT/SIGTERM/SIGCHLD → main loop)
              └──────────────► ControlServer thread (Unix socket IPC)      SysfsReader (/sys, read-only)
```

| Module | Responsibility |
|---|---|
| `ConfigParser` / `service_config` | read + validate configuration; never starts processes |
| `DependencyGraph` | missing deps, cycles, topological order in O(V+E); never launches anything |
| `ProcessManager` | `fork`, `execvp`, `waitpid`, `kill`; stateless; exec failures reported through a CLOEXEC pipe |
| `ProcessRegistry` | service ↔ PID ↔ state ↔ restart count (mutex-protected leaf lock) |
| `RestartPolicy` | restart decision, delay, sliding-window crash-loop detection |
| `HealthMonitor` | background thread; multi-level `/proc` checks; reports unhealthy services |
| `ProcReader` / `SysfsReader` | robust `/proc/<pid>/{stat,status,cmdline}` and read-only `/sys` access |
| `SignalPipe` | async-signal-safe self-pipe bridge from signal handlers to the main loop |
| `ControlServer` | Unix-socket IPC for the CLI |
| `ProcessSupervisor` | orchestration, lifecycle, shutdown ordering |
| `Logger` | thread-safe console + file logging with timestamp, level, component, PID |

More detail: [`docs/`](docs).

### Lifecycle state machine

```
STOPPED → STARTING → RUNNING → STOPPING → STOPPED
RUNNING → FAILED → RESTARTING → STARTING → RUNNING
FAILED  → PERMANENTLY_FAILED     (restart budget exhausted)
```
Illegal transitions are rejected by `ProcessRegistry::transition()` (see `isValidTransition`).

### Signal handling design

Signal handlers may only call async-signal-safe functions. The handlers here do exactly one thing:
`write()` the signal number as one byte into a non-blocking pipe (`SignalPipe`). The supervisor's main loop
`poll()`s the read end and performs all real work (`waitpid`, logging, state changes, shutdown) in ordinary
thread context. `SIGCHLD` bytes are only a *hint*: the loop reaps every finished child on each wake-up, so
coalesced or lost signals cannot leave zombies. Children get their own process group and a clean signal
mask, so a terminal Ctrl-C reaches only the supervisor, which then stops services in reverse dependency order.

### Linux device-driver concepts (no kernel module)

The project is strictly C++20 userspace, so it does **not** implement a kernel driver. Drivers publish device
state through kernel interfaces; Process Pilot Pro consumes them read-only: `/proc` (per-process kernel
state) and `/sys` (the device model: `/sys/devices/system/cpu/online`, `/sys/class/thermal`, `/sys/class/net`).
`SysfsReader` supplies the system line of `process-pilot monitor` (online CPUs, frequency and temperature when
the hardware exposes them).

### Health monitoring levels

0. existence (`/proc/<pid>` readable) 1. kernel state (`Z` zombie, `T` stopped) 2. ownership (`PPid` equals the
supervisor, detects PID reuse) 3. resource limits (`VmRSS` vs `max_rss_kb`) - plus CPU % from `utime+stime`
deltas for `monitor`. Plain crashes are caught immediately through `SIGCHLD`/`waitpid`; repeated crashes by the
`RestartPolicy` window.

### Complexity

Topological sort `O(V+E)` (Kahn, hash-map adjacency); service/PID lookup `O(1)` average; health checks scan
the registry once per tick, `O(N)`.

## Testing

`ctest --test-dir build` runs 6 suites with a small in-tree harness (`tests/test_framework.hpp`):
config parsing, dependency graph (chains, diamonds, cycles, 2000-node graph), registry/state machine,
restart policy (incl. sliding window), `/proc` + `/sys` parsing on real and fake trees, and
integration tests that fork/exec the real `demo-service` (ordered start/stop, exec failure rollback,
crash → restart → `PERMANENTLY_FAILED`, SIGKILL recovery, SIGTERM→SIGKILL escalation, health-monitor-driven
restart, control-socket commands). `tools/demo-service` is a small C++ program used as a controllable service.

## Project layout

```
include/ + src/   cli  config  dependency  process  restart  monitor  linux  supervisor  logging  common
tests/            config dependency process restart monitor integration (+ test_framework.hpp)
tools/            demo_service.cpp
config/           example.conf  test.conf  cycle.conf
docs/             architecture, lifecycle, dependency resolution, linux integration, testing
```

## Limitations

Foreground supervisor only (no daemonization); one supervisor per control socket; restart of a service does not
restart its dependents; services inherit stdout/stderr of the supervisor (stdin is `/dev/null`).

## License

MIT
