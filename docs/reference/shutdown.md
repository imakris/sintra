# sintra::shutdown

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
bool shutdown();
bool shutdown(const shutdown_options& options);
```

Description: Run the standard collective terminal protocol for the
multi-process Sintra lifecycle. All live participants enter the same
shutdown protocol; the runtime owns the rendezvous, optional coordinator
hook, drain wait, and teardown. The first form is a symmetric collective
shutdown. The overload that takes [`sintra::shutdown_options`](shutdown_options.md)
lets the coordinator run a bounded side effect inside the protocol.

## Parameters

- `options` (second overload) — controls coordinator-side behaviour. Pass a
  `coordinator_shutdown_hook` to run a bounded side effect (write a summary
  file, flush metrics) at a defined point inside the protocol.

## Returns

- `true` when teardown completed locally.
- `false` when no managed process was active to tear down.

## Throws

- `std::logic_error` — when called while another lifecycle teardown is
  already in progress.
- Exceptions thrown by `coordinator_shutdown_hook` propagate to the caller
  of `shutdown(options)` after the local process is finalised.

## Use when

- All swarm participants reach the same top-level handoff and are ready to
  tear down together.
- The coordinator must run a bounded side effect at a defined point inside
  the protocol; pass a `shutdown_options` with a `coordinator_shutdown_hook`.
- A single-process program needs the public terminal API. With no
  coordinator id present, `shutdown` falls through to the internal
  finalisation path without a collective barrier.

## Contract

- `shutdown()` is equivalent to `shutdown(shutdown_options{})`.
- The collective protocol runs three steps:
  1. A processing-fence barrier on the `_sintra_all_processes` group.
  2. The coordinator runs `coordinator_shutdown_hook` if set, while
     non-coordinator participants wait inside the protocol.
  3. The coordinator drain-waits for peers to exit the group, then every
     participant tears down its local managed process.
- A second `shutdown` call must not be issued while a teardown is already
  in progress.
- Ordinary callers must not pair `shutdown` with extra final
  `_sintra_all_processes` user barriers, additional finalisation calls, or
  duplicated `shutdown` invocations.
- After successful return, the local Sintra runtime is torn down; facade
  APIs are no longer usable until a new [`sintra::init`](init.md) is
  performed.

## Threading and lifecycle

- Call from a top-level control thread, never from inside a message
  handler or post-handler callback. The protocol claims teardown admission
  state synchronously.
- The collective barrier blocks until every live participant arrives.
  Participants that have already left via [`sintra::leave`](leave.md) or
  have exited abnormally are excluded from the membership snapshot.
- If the coordinator hook throws, the runtime still finalises the local
  process before rethrowing the exception.

## Notes

- A barrier RPC failure inside the protocol is treated as satisfied during
  shutdown handling and does not abort teardown.

## Example source

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_5_barrier_flush.cpp](../../example/sintra/sintra_example_5_barrier_flush.cpp)
- [tests/shutdown_options_test.cpp](../../tests/shutdown_options_test.cpp)
- [tests/shutdown_helper_test.cpp](../../tests/shutdown_helper_test.cpp)
- [tests/shutdown_options_throwing_hook_test.cpp](../../tests/shutdown_options_throwing_hook_test.cpp)

## See also

- [`sintra::init`](init.md)
- [`sintra::leave`](leave.md)
- [`sintra::shutdown_options`](shutdown_options.md)
- [`sintra::barrier`](barrier.md)
- [docs/barriers_and_shutdown.md](../barriers_and_shutdown.md)
