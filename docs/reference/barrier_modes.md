# Barrier modes

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

Sintra exposes three barrier strength tags that select what happens after
all participants reach the named barrier. They are template parameters of
`barrier()` and are tag types only (no fields). Stronger modes do more work
on top of weaker ones; all participants in one barrier round must use the
same mode.

Signature:

```cpp
struct rendezvous_t       {};
struct delivery_fence_t   {};
struct processing_fence_t {};

template <typename BarrierMode = delivery_fence_t>
sequence_counter_type barrier(
    const std::string& barrier_name,
    const std::string& group_name = "_sintra_external_processes");
```

Use when:

- `rendezvous_t` is sufficient when the caller only needs to know that
  every participant has reached the same checkpoint. It does not flush the
  local request reader.
- `delivery_fence_t` (the default) is the right choice when subsequent
  local code must observe every pre-barrier message that was destined for
  this process. It performs the rendezvous and then waits until the local
  request reader has drained up to the barrier sequence.
- `processing_fence_t` is the strongest mode. After the rendezvous, every
  participant waits until their local handlers have run to completion for
  every pre-barrier message, and then a second rendezvous confirms that
  state across the group. Use it when later code must observe handler side
  effects on every participant.

Contract:

- The default `BarrierMode` is `delivery_fence_t`. Calls written as
  `sintra::barrier("name")` request a delivery fence.
- `delivery_fence_t` only proves draining for the calling process. It does
  not prove that peers also drained their inbound traffic; use
  `processing_fence_t` for cross-participant proof.
- `processing_fence_t` issues a second internal rendezvous under a name
  prefixed with `_sintra_processing_phase/`. That name is internal and is
  not part of the user-visible barrier namespace.
- Mixing modes on the same `(barrier_name, group_name)` round is rejected
  immediately by the coordinator, not silently joined.
- Each mode returns the same shape: `sequence_counter_type`. Use
  `barrier_completed(seq)` to distinguish a normal completion from
  `invalid_sequence`, which is returned when a barrier was treated as
  satisfied during shutdown or drain handling.

Threading and lifecycle:

- A `processing_fence_t` barrier is the heaviest. It blocks until both the
  local request stream and every peer's processing have caught up. Avoid
  it on hot paths that fire frequently.
- A `delivery_fence_t` barrier waits on the local delivery fence after the
  rendezvous. The wait is bounded by the same teardown semantics as other
  Sintra waits.
- Calling any mode from a message handler risks a deadlock; use a control
  thread.

Failures:

- Same as [`sintra::barrier`](barrier.md). Modes do not introduce
  additional failure paths beyond the rendezvous and fence steps they
  perform.

Example source:

- [example/sintra/sintra_example_5_barrier_flush.cpp](../../example/sintra/sintra_example_5_barrier_flush.cpp)
- [tests/processing_fence_test.cpp](../../tests/processing_fence_test.cpp)
- [tests/barrier_delivery_fence_repro_test.cpp](../../tests/barrier_delivery_fence_repro_test.cpp)
- [tests/barrier_complex_choreography_test.cpp](../../tests/barrier_complex_choreography_test.cpp)

See also:

- [sintra::barrier](barrier.md)
- [docs/barriers_and_shutdown.md](../barriers_and_shutdown.md)
