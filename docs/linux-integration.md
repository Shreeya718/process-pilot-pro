# Linux integration

| Interface | Used for |
|---|---|
| `fork`, `execvp`, `waitpid`, `kill`, `setpgid` | process creation, reaping, signalling (process groups) |
| `sigaction` + self-pipe (`pipe2`, `poll`) | async-signal-safe SIGINT/SIGTERM/SIGCHLD handling |
| `/proc/<pid>/stat` | state, PPID, utime/stime, threads, starttime, RSS (parser anchors on the last `)` because `comm` may contain spaces/parentheses) |
| `/proc/<pid>/status` | `VmRSS`, `VmSize`, `Threads` |
| `/proc/<pid>/cmdline` | NUL-separated argv |
| `/sys/devices/system/cpu/online`, `cpu0/cpufreq`, `/sys/class/thermal`, `/sys/class/net` | read-only hardware / device-model information |
| `AF_UNIX` sockets | CLI ↔ supervisor IPC |
| `O_CLOEXEC`, `MSG_NOSIGNAL` | no fd leaks into children, no SIGPIPE |

`ProcReader` handles: missing `/proc/<pid>` (`ProcessNotFound`), permission errors (`PermissionDenied`), a process
disappearing between the `stat`, `status` and `cmdline` reads, short reads, malformed content and unexpected states.
`SysfsReader` rejects `..` in attribute paths and treats every optional attribute as optional.

No kernel module is implemented (the project is C++20 userspace only): drivers expose their state through procfs and
sysfs, and this project consumes those interfaces.
