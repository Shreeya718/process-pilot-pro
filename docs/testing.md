# Testing

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -DPROCESS_PILOT_SANITIZERS=ON && cmake --build build-asan -j && ctest --test-dir build-asan
```

| Suite | Covers |
|---|---|
| `test_config` | valid/invalid syntax, defaults, quoting, line numbers in errors |
| `test_dependency` | chain, diamond, ordering ties, cycles, self-cycle, missing/duplicate, 2000-node graph |
| `test_registry` | registration, PID index, legal/illegal transitions, snapshot order |
| `test_restart` | never / on_failure / always, limits, sliding window, reset |
| `test_monitor` | `/proc` parsing (real and fake trees), health rules, CPU %, `/sys` parsing, prompt thread stop |
| `test_integration` | real fork/exec of `demo-service`: ordering, rollback, crash loop, SIGKILL recovery, SIGKILL escalation, memory-limit restart, control socket |

The harness is a ~60-line header (`tests/test_framework.hpp`); there are no third-party dependencies.
`tools/demo_service.cpp` is a controllable C++ service (`run`, `sleep`, `crash`, `exit`, `stubborn` modes).
