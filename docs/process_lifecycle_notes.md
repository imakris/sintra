# Process lifecycle and recovery hooks

This note documents the lifecycle and recovery callbacks that take effect in
the coordinator process, introduced in `lifecycle_types.h` and wired through
`Coordinator`. These hooks are optional; if no handler is configured, recovery
defaults to immediate respawn when enabled.

## Scope and entry points

The following functions are effective only in the coordinator process. Calls
from other processes are no-ops (see `detail/runtime.h`):

- `sintra::set_recovery_policy(...)`
- `sintra::set_recovery_runner(...)`
- `sintra::set_lifecycle_handler(...)`

## How lifecycle events are produced

The coordinator emits lifecycle events in these paths:

1. **Crash**
   - The managed process crash handler calls `Coordinator::note_process_crash`.
   - `note_process_crash` records the crash status and emits a
     `process_lifecycle_event{reason::crash}` immediately.
   - The process is then unpublished; the recorded crash status suppresses a
     duplicate normal-exit/unpublished event.

2. **Normal exit**
   - `sintra::finalize()` calls `Coordinator::begin_process_draining`, which
     sets the draining bit for the process slot.
   - When the process later unpublishes, the coordinator sees the draining bit
     and emits `process_lifecycle_event{reason::normal_exit}`.

3. **Unpublished without draining/crash**
   - If a process is unpublished without a prior crash status and without the
     draining bit set, the coordinator emits
     `process_lifecycle_event{reason::unpublished}`.

The lifecycle handler is called once per event; it is not called for every
unpublish when a crash was already recorded.

## Recovery flow

Recovery is opt-in per process via `sintra::enable_recovery()`. When a crash or
unpublished event occurs, the coordinator executes:

1. `Coordinator::recover_if_required(info)`
2. If the process did not opt in, return immediately.
3. If a `Recovery_policy` is configured and returns `false`, skip recovery.
   When no policy is configured, recovery proceeds by default.
4. If a `Recovery_runner` is configured, the coordinator starts a recovery
   thread and invokes the runner with a `Recovery_control` containing:
   - `should_cancel()` (true when shutdown has begun)
   - `spawn()` (respawns the process once)
5. The runner owns any delay, countdown, or gating logic and decides when to
   call `spawn()`.
6. If no runner is configured, the coordinator respawns immediately.

`Coordinator::begin_shutdown()` flips the shutdown flag so `should_cancel()`
becomes true and recovery threads can exit early.

## Threading expectations

- `Recovery_policy` and `Lifecycle_handler` run on the coordinator thread.
- `Recovery_runner` runs on a recovery thread.
- `Recovery_control::should_cancel()` reflects the coordinator shutdown flag.

All handlers must be thread-safe.

## Typical use cases

- **Crash vs normal exit UI**: show a "crashed" vs "exited" message per slot.
- **Delayed respawn with countdown**: block in the runner, broadcasting ticks.
- **Custom gating**: wait on external conditions before calling `spawn()`.
