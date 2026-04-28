# sintra::leave

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
bool leave();
```

Description: Tear down the local Sintra runtime as a clean unilateral
departure while peers continue running. `leave` performs the same drain,
pause, unpublish, and finalise sequence as the internal teardown path used
by [`sintra::shutdown`](shutdown.md), but it does not enter the collective
`_sintra_all_processes` shutdown protocol.

## Returns

- `true` when teardown completed locally.
- `false` when no managed process was active to tear down.

## Throws

- `std::logic_error` — when called from inside a message handler or
  post-handler callback.
- `std::logic_error` — when the calling coordinator is not the sole
  remaining known process. Use [`sintra::shutdown`](shutdown.md) for
  collective termination.
- `std::logic_error` — when another lifecycle teardown is already in
  progress.
- Other exceptions raised by internal teardown propagate after the protocol
  state has been reset to idle.

## Use when

- One participant needs to exit intentionally and the rest of the topology
  should continue.
- A leaf-like worker has completed its job and is not relying on owned
  descendants to outlive it.
- A coordinator is the sole remaining known process and is ready to tear
  down on its own terms.

## Contract

- Must be called from a top-level control thread, not from a message
  handler or post-handler callback.
- A coordinator may call `leave` only when it is already the sole remaining
  known process.
- A second `leave` call must not be issued while a teardown is already in
  progress.
- After successful return, the local Sintra runtime is torn down; the
  facade APIs are no longer usable until a new [`sintra::init`](init.md)
  is performed.

## Threading and lifecycle

- The implementation announces draining to the coordinator, flushes the
  reply ring up to the watermark, switches the local readers to service
  mode, deactivates handlers, unpublishes transceivers, and destroys the
  managed process.
- Other participants observe a normal-exit lifecycle event for this process
  via [`sintra::set_lifecycle_handler`](lifecycle_hooks.md).

## Notes

- During the call, Sintra reserves lifecycle teardown admission so nested
  `shutdown`, `leave`, or finalisation attempts are rejected.

## Example source

- [tests/leave_lifecycle_test.cpp](../../tests/leave_lifecycle_test.cpp)
- [tests/leave_coordinator_guardrails_test.cpp](../../tests/leave_coordinator_guardrails_test.cpp)
- [tests/lifecycle_guardrails_test.cpp](../../tests/lifecycle_guardrails_test.cpp)

## See also

- [`sintra::init`](init.md)
- [`sintra::shutdown`](shutdown.md)
- [`sintra::set_lifecycle_handler`](lifecycle_hooks.md)
- [docs/barriers_and_shutdown.md](../barriers_and_shutdown.md)
- [docs/process_lifecycle_notes.md](../process_lifecycle_notes.md)
