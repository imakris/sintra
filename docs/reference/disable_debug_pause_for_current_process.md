# sintra::disable_debug_pause_for_current_process

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
void disable_debug_pause_for_current_process() noexcept;
```

Description: Disable the debug pause-on-exit behaviour for the current
process. When Sintra is built with `SINTRA_DEBUG_PAUSE_ON_EXIT`, processes
hold at exit so a debugger can be attached. Call this from a process that
should opt out — typically a long-running headless worker or a test that
must terminate cleanly.

## Returns

- `void`.

## Throws

- Does not throw.

## Use when

- A specific process in the swarm should not pause at exit even though the
  build was configured with debug pause-on-exit.

## Contract

- Must be called from inside the process that is opting out. The opt-out
  applies to the calling process only and does not propagate to peers.
- May be called before or after [`sintra::init`](init.md) in the current
  process.

## Threading and lifecycle

- The opt-out lasts for the lifetime of the current process. There is no
  paired re-enable call.

## Notes

- The pause behaviour is a debug-build feature controlled by
  `SINTRA_DEBUG_PAUSE_ON_EXIT`. In a release build the call is a no-op.

## Example source

- [tests/recovery_test.cpp](../../tests/recovery_test.cpp)
- [tests/recovery_policy_test.cpp](../../tests/recovery_policy_test.cpp)
- [tests/lifecycle_handler_test.cpp](../../tests/lifecycle_handler_test.cpp)

## See also

- [`sintra::init`](init.md)
- [`sintra::shutdown`](shutdown.md)
- [`sintra::leave`](leave.md)
