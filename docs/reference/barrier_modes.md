# Barrier modes

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
struct rendezvous_t       {};
struct delivery_fence_t   {};
struct processing_fence_t {};

template <typename BarrierMode = delivery_fence_t>
sequence_counter_type barrier(
    const std::string& barrier_name,
    const std::string& group_name = "_sintra_external_processes");
```

Description: Three tag types that select the strength of the fence applied
after [`sintra::barrier`](barrier.md) reaches its rendezvous. They are
template parameters of `barrier()` and carry no fields. Stronger modes do
more work on top of weaker ones; all participants in one barrier round
must use the same mode.

## Returns

Each mode causes `sintra::barrier` to return the same shape:

- `sequence_counter_type` — the reply-ring watermark for the completed
  barrier.
- `invalid_sequence` — returned when the barrier was treated as satisfied
  during shutdown or drain handling. Use `seq != invalid_sequence` to
  distinguish a normal completion.

## Throws

- Same as [`sintra::barrier`](barrier.md). Modes do not introduce
  additional failure paths beyond the rendezvous and fence steps they
  perform.

## Use when

- `rendezvous_t` is sufficient when the caller only needs to know that
  every participant has reached the same checkpoint. It does not flush
  the local request reader.
- `delivery_fence_t` (the default) is the right choice when subsequent
  local code must observe every pre-barrier message destined for this
  process. It performs the rendezvous and then waits until the local
  request reader has drained up to the barrier sequence.
- `processing_fence_t` is the strongest mode. After the rendezvous, every
  participant waits until its local delivery/processing fence has caught
  up; a second rendezvous then confirms that state across the group. Use
  it from control threads when later code must observe handler side
  effects on every participant.

## Contract

- The default `BarrierMode` is `delivery_fence_t`. Calls written as
  `sintra::barrier("name")` request a delivery fence.
- `delivery_fence_t` proves draining only for the calling process. It does
  not prove that peers also drained their inbound traffic; use
  `processing_fence_t` for cross-participant proof.
- Mixing modes on the same `(barrier_name, group_name)` round is rejected
  immediately by the coordinator and must not be relied upon as a way to
  upgrade or downgrade an in-flight barrier.

## Threading and lifecycle

- Prefer calling barriers from top-level control threads. Calling
  `rendezvous_t` or `delivery_fence_t` from inside a message handler
  risks a deadlock unless the application protocol proves the blocked
  reader is not needed for peers to reach the barrier.
- When called from a request-reader handler or post-handler,
  `processing_fence_t` is reentrancy-aware: it skips the currently
  executing request reader and may run queued post-handlers while waiting.
  It does not wait for the current handler/post-handler, or for messages
  queued behind it on that same request-reader stream.
- A `processing_fence_t` barrier is the heaviest. It blocks until both the
  local request stream and every peer's processing have caught up. Avoid
  it on hot paths that fire frequently.
- A `delivery_fence_t` barrier waits on the local delivery fence after the
  rendezvous. The wait is bounded by the same teardown semantics as other
  Sintra waits.

## Notes

- `processing_fence_t` issues a second internal rendezvous under a name
  prefixed with `_sintra_processing_phase/`. That prefix is internal and
  is not part of the user-visible barrier namespace.

## Example source

- [example/sintra/sintra_example_5_barrier_flush.cpp](../../example/sintra/sintra_example_5_barrier_flush.cpp)
- [tests/processing_fence_test.cpp](../../tests/processing_fence_test.cpp)
- [tests/barrier_delivery_fence_repro_test.cpp](../../tests/barrier_delivery_fence_repro_test.cpp)
- [tests/barrier_complex_choreography_test.cpp](../../tests/barrier_complex_choreography_test.cpp)

## See also

- [`sintra::barrier`](barrier.md)
- [docs/barriers_and_shutdown.md](../barriers_and_shutdown.md)
