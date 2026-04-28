# sintra::barrier

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
template <typename BarrierMode = delivery_fence_t>
sequence_counter_type barrier(
    const std::string& barrier_name,
    const std::string& group_name = "_sintra_external_processes");

bool barrier_completed(sequence_counter_type barrier_sequence);
```

Description: Block the calling thread until every live member process in
`group_name` has reached the same named barrier, then optionally enforce
a delivery or processing fence according to the selected
[barrier mode](barrier_modes.md). The default mode is `delivery_fence_t`.
The companion helper `barrier_completed(seq)` reports whether the
returned sequence describes a successful collective barrier.

## Parameters

- `barrier_name` — the name shared by every participant for this round.
  Must not begin with the reserved `_sintra_` prefix.
- `group_name` — the named process group whose live members must arrive
  before the barrier completes. Defaults to `_sintra_external_processes`,
  which excludes the coordinator. Use `_sintra_all_processes` to include
  the coordinator (subject to the teardown restriction in *Contract*).
- `BarrierMode` (template parameter) — selects the strength of the fence
  applied after the rendezvous. See
  [barrier modes](barrier_modes.md).

## Returns

- `sequence_counter_type` — the reply-ring watermark for the completed
  barrier.
- `invalid_sequence` — returned when the barrier was treated as satisfied
  during shutdown or drain handling. The bypass is logged as a warning so
  callers can proceed without re-entering teardown. Use
  `barrier_completed(seq)` to test for a normal completion.

## Throws

- `std::invalid_argument` — when `barrier_name` starts with the reserved
  `_sintra_` prefix.
- `std::logic_error` — when called on `_sintra_all_processes` while a
  lifecycle teardown is in progress.
- Other coordinator-side errors propagate as the original exception type.

## Use when

- Synchronising swarm participants on a named checkpoint without
  exchanging custom rendezvous messages.
- Establishing a happens-before relationship between phases (for example,
  ensuring slot activations before a sender broadcasts).
- Coordinating a group that includes the coordinator, the external
  processes, or any registered group such as the default
  `_sintra_external_processes`.

## Contract

- All participants must use the same `barrier_name`, the same `group_name`,
  and the same `BarrierMode`. Mixing modes on the same barrier round is
  rejected immediately rather than hanging in the hidden processing phase.
- Barrier names beginning with `_sintra_` are reserved for internal runtime
  protocols and must not be used by callers.
- Barrier rounds track processes, not calling threads. For a single
  `(barrier_name, group_name, BarrierMode)` round, each process should have
  at most one in-flight caller. If multiple threads in one process must
  wait for the same phase, coordinate them with normal threading
  primitives and have one representative enter `sintra::barrier`, or use
  distinct barrier names for independent rounds.

## Threading and lifecycle

- Prefer calling barriers from top-level control threads. Calling
  `rendezvous_t` or `delivery_fence_t` from inside a slot or post-handler
  callback risks a deadlock unless the application protocol proves the
  blocked reader is not needed for peers to reach the barrier.
- The default mode `delivery_fence_t` proves that the calling process
  drained its readers up to the barrier point. It does not perform a
  second rendezvous proving that peers also finished draining; use
  `processing_fence_t` for that stronger guarantee.
- A `processing_fence_t` barrier called from a request-reader handler or
  post-handler is reentrancy-aware: it skips the currently executing
  request reader and may run queued post-handlers while waiting. It does
  not wait for the current handler/post-handler, or for messages queued
  behind it on that same request-reader stream. Use a control thread when
  the fence must include that work.

## Example source

- [example/sintra/sintra_example_5_barrier_flush.cpp](../../example/sintra/sintra_example_5_barrier_flush.cpp)
- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [tests/barrier_flush_test.cpp](../../tests/barrier_flush_test.cpp)
- [tests/barrier_guardrails_test.cpp](../../tests/barrier_guardrails_test.cpp)

## See also

- [Barrier modes](barrier_modes.md)
- [`sintra::shutdown`](shutdown.md)
- [`sintra::leave`](leave.md)
