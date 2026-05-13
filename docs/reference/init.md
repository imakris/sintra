# sintra::init

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
void init(
    int argc,
    const char* const* argv,
    std::vector<Process_descriptor> branches = std::vector<Process_descriptor>());

template <typename... Args>
void init(int argc, const char* const* argv, Args&&... args);
```

Description: Initialise the current process as a member of a Sintra swarm.
`init` records the program arguments, creates the per-process
`Managed_process` singleton, branches the requested child processes (when
called from the starter), and connects the local process to the coordinator.
After it returns successfully, the high-level facade (transceivers, RPC,
slots, barriers, lifecycle helpers) is usable.

## Parameters

- `argc`, `argv` — the original program arguments from `main`. Both must be
  passed unchanged. Sintra appends its own `--swarm_id`, `--instance_id`, and
  `--coordinator_id` arguments for children it spawns and for external
  process invitations.
- `branches` (vector overload) — the swarm topology declared by the starter.
  Each `Process_descriptor` becomes one spawned process; the index of the
  descriptor in the vector becomes the branch index of that process.
- `Args&&... args` (variadic overload) — descriptors and per-descriptor
  multiplicities forwarded through `make_branches(...)` to the vector
  overload. An integer in the pack is read as a multiplicity that applies
  to the next descriptor.

## Returns

- `void`. After successful return, `process_index()` returns the branch index
  of the current process.

## Throws

- `sintra::init_error` — when one or more child processes cannot be spawned
  or fail to reach the initialisation barrier. The exception's `failures()`
  and `successful_spawns()` describe what happened, and `diagnostic_report()`
  returns a formatted multi-line summary.
- `std::runtime_error` — when called twice in the same process without a
  matching teardown.
- Other exceptions during managed-process startup propagate as the original
  exception type from the runtime.

## Use when

- The current `main()` is the entry point of a Sintra-managed process.
- The starter process needs to declare the swarm topology by passing
  `Process_descriptor` values directly, an existing branch vector built with
  `make_branches`, or a mix of descriptors and per-descriptor multiplicities.
- A spawned child process attaches to the existing swarm. The variadic forms
  remain valid in the child; branch arguments whose role is only meaningful
  in the starter are ignored.
- A manually launched process has received arguments from
  `External_process_invitation::sintra_args()` and must claim that invitation
  before user-level Sintra work begins.

## Contract

- Must be called exactly once per process before any other facade API.
- The starter process is the one that observes its branch index as `0`.
  Each declared branch becomes a spawned process selected by that branch
  index.
- A second `init` call without an intervening `shutdown()`, `leave()`, or
  `detail::finalize()` throws `std::runtime_error`.
- The variadic overload forwards through `make_branches(...)` and reaches
  the vector-based overload.

## Threading and lifecycle

- Call `init` from the program's main thread before spawning user threads
  that depend on the Sintra runtime.
- On Linux and macOS, `init` ignores `SIGPIPE` and starts a new session via
  `setsid()`. Sintra also installs handlers for `SIGABRT`, `SIGFPE`,
  `SIGILL`, `SIGINT`, `SIGSEGV`, and `SIGTERM`, chaining any pre-existing
  handlers where possible. Programs that need bespoke signal handling must
  install their handlers after `init` returns.

## Notes

- `init` may install debug-pause handlers that hold the process at exit
  when the `SINTRA_DEBUG_PAUSE_ON_EXIT` build configuration is enabled.
  Call [`disable_debug_pause_for_current_process`](disable_debug_pause_for_current_process.md)
  from the current process to opt out.
- The cache-line check is debug-only. In `Debug` builds, `init` logs a
  warning when the assumed cache-line size does not match the host's actual
  size; the check is suppressed in `Release` builds.

## Example source

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_4_recovery.cpp](../../example/sintra/sintra_example_4_recovery.cpp)
- [example/sintra/sintra_example_5_barrier_flush.cpp](../../example/sintra/sintra_example_5_barrier_flush.cpp)
- [tests/init_error_spawn_failure_test.cpp](../../tests/init_error_spawn_failure_test.cpp)

## See also

- [`sintra::make_branches`](make_branches.md)
- [`sintra::Process_descriptor`](process_descriptor.md)
- [`sintra::process_index`](process_index.md)
- [`sintra::create_external_process_invitation`](external_process_invitation.md)
- [`sintra::shutdown`](shutdown.md)
- [`sintra::leave`](leave.md)
- [`sintra::init_error`](init_error.md)
- [`sintra::disable_debug_pause_for_current_process`](disable_debug_pause_for_current_process.md)
