# sintra::enable_recovery / set_recovery_policy / set_recovery_runner

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
void enable_recovery();

void set_recovery_policy(Recovery_policy policy);
void set_recovery_runner(Recovery_runner runner);

using Recovery_policy = std::function<bool(const Crash_info&)>;
using Recovery_runner =
    std::function<void(const Crash_info&, const Recovery_control&)>;

struct Crash_info
{
    instance_id_type process_iid = invalid_instance_id;
    uint32_t         process_slot = 0;
    int              status = 0;
};

struct Recovery_control
{
    std::function<bool()> should_cancel;
    std::function<void()> spawn;
};

extern uint32_t s_recovery_occurrence; // process-local
```

Description: The recovery API opts a managed-child custody into automatic
respawn after abnormal exit and configures the coordinator-side decision and
execution callbacks. `enable_recovery()` is called by the candidate child. The
coordinator then decides whether to respawn through
`set_recovery_policy(...)` and performs any custom delay or gating in a
`set_recovery_runner(...)` callback. The detected recovery occurrence
count is exposed through the process-local global
`s_recovery_occurrence`. The value is relative to the current managed-child
custody, not a process-instance-id generation counter.

## Parameters

- `policy` - coordinator-side predicate that decides whether a crash event
  should be recovered.
- `runner` - coordinator-side callback that controls when the respawn is
  performed.

## Returns

- `void`.

## Throws

- `enable_recovery()` throws `std::runtime_error` when called before the
  local managed process has been created by [`sintra::init`](init.md), or
  after it has been torn down.
- `set_recovery_policy` and `set_recovery_runner` do not throw
  Sintra-defined exceptions. Calls from non-coordinator processes are
  no-ops.

## Use when

- A worker should restart automatically on crash. Call
  `enable_recovery()` from inside that worker's entry function. Branch on
  `s_recovery_occurrence` to distinguish the original run (`0`) from later
  recoveries (`1`, `2`, ...).
- A coordinator wants to filter recovery decisions through a policy that
  inspects `Crash_info::status`, the process slot, or external state.
  Configure the policy before the swarm reaches a state where crashes
  matter.
- The recovery action should not be immediate respawn. Configure a runner;
  the runner runs on a background thread, can sleep, broadcast countdown
  messages, and call `Recovery_control::spawn()` exactly once to trigger
  the actual respawn.

## Contract

- `enable_recovery()` operates on the local managed process. It marks the
  exact custody identified by that process's authenticated message context.
  The custody's structured launch recipe was committed before its original
  child or publication became visible; recovery reuses that recipe rather than
  reconstructing a command from the process instance id.
- Recovery authority is custody-scoped. Once enabled, it is inherited by later
  occurrences of the same custody. A fresh custody starts with recovery
  disabled, even if another custody in the active runtime used the same process
  instance id. Externally attached processes have no managed custody and are
  never recovered; calling `enable_recovery()` from one has no effect and logs
  a warning.
- `set_recovery_policy` and `set_recovery_runner` only take effect inside
  the coordinator process. Calls from non-coordinator processes are no-ops.
- The default behaviour, when no policy is configured, is to recover the
  process. A policy that returns `false` from a crash-info inspection skips
  the recovery for that event.
- The default runner, when none is configured, schedules the spawn on a
  coordinator-owned recovery thread. A custom runner runs on its own recovery
  thread and decides when to call `Recovery_control::spawn()`. Calling
  `spawn()` more than once is ignored.
- Crash handling captures the exact custody, predecessor occurrence, and
  structured launch recipe before policy or runner code runs. A retained
  `Recovery_control::spawn()` revalidates that the captured custody remains
  open, opted in, and still owns that predecessor. It is inert after custody
  release or retirement, process-id reuse, or runtime teardown, and cannot
  redirect recovery to a newer custody.
- `Recovery_control::should_cancel()` becomes `true` when the coordinator
  has begun shutting down. Runners must check it and return promptly to let
  teardown progress.
- `s_recovery_occurrence` is set in the recovered process at startup before
  the entry function runs. It is `0` for the first run, `1` for the first
  recovery, and so on. Every fresh custody starts at `0`, even when another
  custody in the same runtime previously used the same process instance id or
  when the process was added by a mid-flight swarm join. Reading the value from
  a non-recoverable or non-spawned context is meaningful only after `init` has
  set the local managed process up.
- A recovery event is also surfaced through
  [`sintra::set_lifecycle_handler`](lifecycle_hooks.md) as a
  `process_lifecycle_event::reason::crash` when the process crashed. The
  lifecycle callback runs after exact publication retirement and notification
  enqueueing; recovery is considered only after that callback returns.

## Threading and lifecycle

- The recovery policy is invoked on the coordinator thread that handles the
  crash event. Keep the work cheap and side-effect-free; defer expensive
  logic to the runner.
- The runner runs on a dedicated coordinator-owned thread for the duration
  of one recovery decision. It must be thread-safe relative to other
  coordinator state it touches.
- Default recovery uses the same coordinator-owned recovery-thread mechanism,
  even though it calls `spawn()` immediately on that thread.
- A custody that did not call `enable_recovery()` is not respawned even when a
  policy or runner is configured.

## Failure modes

- Misconfiguration shows up as a missed recovery: the policy returned
  `false`, the runner did not call `spawn`, the custody exited without enabling
  recovery, or a retained control was invoked after its exact custody closed.

## Example source

- [example/sintra/sintra_example_4_recovery.cpp](../../example/sintra/sintra_example_4_recovery.cpp)
- [tests/recovery_policy_test.cpp](../../tests/recovery_policy_test.cpp)
- [tests/recovery_runner_thread_test.cpp](../../tests/recovery_runner_thread_test.cpp)
- [tests/recovery_test.cpp](../../tests/recovery_test.cpp)

## See also

- [`sintra::set_lifecycle_handler`](lifecycle_hooks.md), which surfaces
  crash recovery observations through lifecycle events.
- [docs/process_lifecycle_notes.md](../process_lifecycle_notes.md)
