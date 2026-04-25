# sintra::barrier

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`barrier` is the coordinated rendezvous primitive of Sintra. It blocks the
calling thread until at least one thread of every member process in
`group_name` has reached the same named barrier, then optionally enforces a
delivery or processing fence according to the selected
[barrier mode](barrier_modes.md). The default mode is `delivery_fence_t`.
The companion helper `barrier_completed(seq)` reports whether the returned
sequence describes a successful collective barrier.

Signature:

```cpp
template <typename BarrierMode = delivery_fence_t>
sequence_counter_type barrier(
    const std::string& barrier_name,
    const std::string& group_name = "_sintra_external_processes");

bool barrier_completed(sequence_counter_type barrier_sequence);
```

Use when:

- Synchronising swarm participants on a named checkpoint without exchanging
  custom rendezvous messages.
- Establishing a happens-before relationship between phases (for example,
  ensuring slot activations before a sender broadcasts).
- Coordinating a group that includes the coordinator, the external
  processes, or any registered group such as
  `_sintra_external_processes` (the default).

Contract:

- All participants must use the same `barrier_name`, the same `group_name`,
  and the same `BarrierMode`. Mixing modes on the same barrier round is
  rejected immediately rather than hanging in the hidden processing phase.
- Barrier names beginning with `_sintra_` are reserved for internal runtime
  protocols and are rejected with `std::invalid_argument`.
- The function returns the reply-ring watermark for the completed barrier.
  `barrier_completed(seq)` returns `seq != invalid_sequence`. A return of
  `invalid_sequence` indicates the barrier was treated as satisfied during
  shutdown or drain handling (see Failures below).
- When multiple threads of the same process enter the same barrier, they
  rendezvous with the corresponding threads of the other participants in
  unspecified order.
- Barriers coordinate processes only. Combine with normal threading
  primitives (mutex, condition variable) to gate work between threads of
  one process.

Threading and lifecycle:

- May be called from any user thread that is not currently dispatching a
  message handler. Calling `barrier` from inside a slot or post-handler
  callback risks a deadlock if the same thread is responsible for the
  delivery flow being awaited.
- Calling a user-facing barrier on `_sintra_all_processes` is rejected with
  `std::logic_error` while a lifecycle teardown protocol is active. Use the
  default group `_sintra_external_processes` for non-coordinator barriers,
  or `_sintra_all_processes` only outside teardown.
- The default mode `delivery_fence_t` proves that this process drained its
  readers up to the barrier point. It does not perform a second rendezvous
  proving that peers also finished draining; use `processing_fence_t` for
  that stronger guarantee.

Failures:

- Throws `std::invalid_argument` when `barrier_name` starts with the
  reserved `_sintra_` prefix.
- Throws `std::logic_error` when called on `_sintra_all_processes` while a
  lifecycle teardown is in progress.
- Returns `invalid_sequence` when the underlying coordinator RPC was
  cancelled or the runtime is no longer running normally; the bypass is
  logged as a warning and treated as a satisfied barrier so callers can
  proceed without reentering teardown.
- Other coordinator-side errors propagate as the original exception type.

Example source:

- [example/sintra/sintra_example_5_barrier_flush.cpp](../../example/sintra/sintra_example_5_barrier_flush.cpp)
- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [tests/barrier_flush_test.cpp](../../tests/barrier_flush_test.cpp)
- [tests/barrier_guardrails_test.cpp](../../tests/barrier_guardrails_test.cpp)

See also:

- [Barrier modes](barrier_modes.md)
- [sintra::shutdown](shutdown.md)
- [sintra::leave](leave.md)
