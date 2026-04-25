# sintra::init_error

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`init_error` is the exception thrown by `sintra::init` when one or more
child processes fail to initialise. It carries a structured list of
per-process failures, the instance ids that did spawn successfully (so a
caller can distinguish full failure from partial failure), and a
preformatted diagnostic report suitable for stderr or a log line.

Signature:

```cpp
class init_error : public std::runtime_error {
public:
    enum class cause {
        spawn_failed,
        barrier_timeout,
        ipc_setup_failed
    };

    struct failed_process {
        std::string      binary_name;
        instance_id_type instance_id;
        cause            failure_cause;
        int              errno_value;
        std::string      error_message;
    };

    init_error(
        std::vector<failed_process> failures,
        std::vector<instance_id_type> successful = {});

    const std::vector<failed_process>&    failures() const;
    const std::vector<instance_id_type>&  successful_spawns() const;
    const std::string&                    diagnostic_report() const;
};
```

Use when:

- Catching `sintra::init(...)` to detect and report startup failure
  modes (missing binary, child crashed before barrier, IPC misconfigured).
- Inspecting the structured failure list to make programmatic decisions:
  retry only on `spawn_failed`, escalate on `barrier_timeout`, surface
  IPC errors specially.
- Formatting a multi-line diagnostic block that names every failed
  process, its cause, and any platform `errno` value.

Contract:

- Subclass of `std::runtime_error`. The `what()` summary is short
  ("`sintra::init() failed: N process(es) could not be initialized`");
  use `diagnostic_report()` for the long form.
- `failures()` returns one entry per process that failed. Each entry
  carries the binary name, the instance id, a `cause` enum, the OS
  `errno_value` (set when applicable, otherwise zero), and a
  human-readable message.
- `successful_spawns()` returns the instance ids that did successfully
  spawn, which the caller may use for cleanup or correlation.
- `diagnostic_report()` returns a formatted block that lists every
  successful spawn, every failed entry with cause-specific guidance, and
  generic check items at the end.
- `cause` values:
  - `spawn_failed`: the operating system rejected the spawn attempt (for
    example, missing binary). `errno_value` is populated when available.
  - `barrier_timeout`: the child process was spawned but did not reach
    the initialisation barrier; usually means it crashed during startup.
  - `ipc_setup_failed`: a Sintra IPC channel could not be established.

Threading and lifecycle:

- Catch it on the thread that called `sintra::init`. After the throw,
  the local Sintra runtime is not initialised; do not call any facade
  API until a new `init` succeeds.
- Successfully spawned children listed by `successful_spawns()` may have
  already been torn down as part of the failed init sequence; treat the
  list as informational unless your test logic explicitly tracks them.

Failures:

- The exception itself does not throw on construction or accessor calls.

Example source:

- [tests/init_error_spawn_failure_test.cpp](../../tests/init_error_spawn_failure_test.cpp)

See also:

- [sintra::init](init.md)
- [sintra::Process_descriptor](process_descriptor.md)
