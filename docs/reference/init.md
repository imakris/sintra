# sintra::init

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`init` initialises the current process as a member of a Sintra swarm. It
records the program arguments, creates the per-process `Managed_process`
singleton, branches the requested child processes (when called from the
starter), and connects the local process to the coordinator. After `init`
returns successfully, the high-level facade (transceivers, RPC, slots,
barriers, lifecycle helpers) is usable.

Signature:

```cpp
void init(
    int argc,
    const char* const* argv,
    std::vector<Process_descriptor> branches = std::vector<Process_descriptor>());

template <typename... Args>
void init(int argc, const char* const* argv, Args&&... args);
```

Use when:

- The current `main()` is the entry point of a Sintra-managed process.
- The starter process needs to declare the swarm topology by passing
  `Process_descriptor` values directly, an existing branch vector built with
  `make_branches`, or a mix of descriptors and per-descriptor multiplicities.
- A spawned child process wants to attach to the existing swarm (in that case
  the variadic forms are still valid; the child ignores branch arguments
  whose role is only meaningful in the starter).

Contract:

- Must be called exactly once per process before any other facade API. The
  variadic overload forwards through `make_branches(...)` and reaches the
  vector-based overload.
- The starter process is the one that observes its branch index as `0`. Each
  declared branch becomes a spawned process selected by that branch index.
- Both the original `argc` and `argv` from `main` must be passed unchanged.
  Sintra appends its own `--swarm_id`, `--instance_id`, and
  `--coordinator_id` arguments for the children it spawns.
- When the variadic form mixes integers and descriptors, an integer is read
  as a multiplicity that applies to the next descriptor (see
  `make_branches`).
- After successful return, `process_index()` returns the branch index of the
  current process.

Threading and lifecycle:

- Call `init` from the program's main thread before spawning user threads
  that depend on the Sintra runtime.
- A second `init` call without an intervening `shutdown()`, `leave()`, or
  `detail::finalize()` is rejected with `std::runtime_error` carrying the
  message `"sintra::init() called twice without a matching teardown"`.
- On Linux/macOS, `init` ignores `SIGPIPE` and starts a new session via
  `setsid()`. Sintra also installs handlers for `SIGABRT`, `SIGFPE`,
  `SIGILL`, `SIGINT`, `SIGSEGV`, and `SIGTERM`, chaining any pre-existing
  handlers where possible. Programs that need bespoke signal handling should
  install their handlers after the `init` call returns.
- `init` may install debug-pause handlers that hold the process at exit
  when the `SINTRA_DEBUG_PAUSE_ON_EXIT` build configuration is enabled.
  Call `disable_debug_pause_for_current_process()` from the current
  process to opt out.
- The cache-line check is debug-only. In `Debug` builds, `init` logs a
  warning when the assumed cache-line size does not match the host's actual
  size; the check is suppressed in `Release` builds.

Failures:

- Throws `sintra::init_error` when one or more child processes cannot be
  spawned or fail to reach the initialisation barrier. The exception's
  `failures()` and `successful_spawns()` methods describe what happened, and
  `diagnostic_report()` returns a formatted multi-line summary.
- Throws `std::runtime_error` when called twice in the same process without a
  matching teardown.
- Other failures during managed-process startup propagate as the original
  exception type from the runtime.

Example source:

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_4_recovery.cpp](../../example/sintra/sintra_example_4_recovery.cpp)
- [example/sintra/sintra_example_5_barrier_flush.cpp](../../example/sintra/sintra_example_5_barrier_flush.cpp)
- [tests/init_error_spawn_failure_test.cpp](../../tests/init_error_spawn_failure_test.cpp)

## sintra::disable_debug_pause_for_current_process

Signature:

```cpp
void disable_debug_pause_for_current_process();
```

Disables debug pause-on-exit behavior for the current process. Call it from the
process that should opt out.

See also:

- [sintra::make_branches](make_branches.md)
- [sintra::Process_descriptor](process_descriptor.md)
- [sintra::shutdown](shutdown.md)
- [sintra::leave](leave.md)
- [sintra::init_error](init_error.md)
- [sintra::process_index](process_index.md)
