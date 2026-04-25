# sintra::leave

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`leave` is the public terminal API for a clean unilateral departure: this
process tears down its own Sintra runtime while peers may continue running.
It performs the same local drain, pause, unpublish, and finalise sequence
as the internal teardown path used by `shutdown()`, but it does not enter
the collective `_sintra_all_processes` shutdown protocol.

Signature:

```cpp
bool leave();
```

Use when:

- One participant wants to exit intentionally and the rest of the topology
  should continue.
- A leaf-like worker has completed its job and is not relying on owned
  descendants to outlive it.
- A coordinator is the sole remaining known process and is ready to tear
  down on its own terms.

Contract:

- Must be called from a top-level control thread, not from a message
  handler or post-handler callback. The runtime detects that situation and
  throws `std::logic_error` immediately.
- A coordinator may call `leave()` only when it is already the sole
  remaining known process. Otherwise the call throws `std::logic_error`
  with a message instructing the caller to use `shutdown()` for collective
  termination.
- Returns `true` when teardown completed locally. Returns `false` when no
  managed process was active to tear down.
- After successful return, the local Sintra runtime is torn down and the
  facade APIs are no longer usable until a new `init` is performed.
- A second `leave()` call while a teardown is already in progress is
  rejected: the runtime throws `std::logic_error`.

Threading and lifecycle:

- The implementation announces draining to the coordinator, flushes the
  reply ring up to the watermark, switches the local readers to service
  mode, deactivates handlers, unpublishes transceivers, and destroys the
  managed process. Other participants observe a normal-exit lifecycle
  event for this process via `set_lifecycle_handler`.
- The teardown admission state visible through
  `detail::s_shutdown_state` advances to `local_departure_entered` for
  the duration of the call.

Failures:

- Throws `std::logic_error` when called from inside a handler or
  post-handler callback.
- Throws `std::logic_error` when the calling coordinator is not the sole
  remaining known process.
- Throws `std::logic_error` when another lifecycle teardown is already in
  progress.
- Other exceptions raised by internal teardown propagate after the
  protocol state has been reset to idle.

Example source:

- [tests/leave_lifecycle_test.cpp](../../tests/leave_lifecycle_test.cpp)
- [tests/leave_coordinator_guardrails_test.cpp](../../tests/leave_coordinator_guardrails_test.cpp)
- [tests/lifecycle_guardrails_test.cpp](../../tests/lifecycle_guardrails_test.cpp)

See also:

- [sintra::shutdown](shutdown.md)
- [sintra::lifecycle_hooks](lifecycle_hooks.md)
- [docs/barriers_and_shutdown.md](../barriers_and_shutdown.md)
- [docs/process_lifecycle_notes.md](../process_lifecycle_notes.md)
