# Delivery-fence barrier failures on Windows

Windows CI intermittently reports timeouts in the delivery-fence variants of the
barrier tests. The typical signature is that the coordinator process times out
waiting for an iteration marker even though all workers have already re-entered
the barrier for the next iteration. Recent examples include
`sintra_barrier_flush_test_release` timing out after iteration 10 and
`sintra_complex_choreography_test_debug` exhausting the suite timeout. The
bespoke `barrier_delivery_fence_repro_test` exists precisely because Linux does
not reproduce the issue while Windows does. The coordinator in that test sleeps
between marker deliveries and fails as soon as one of the bursts is missing a
sequence number.【F:tests/barrier_flush_test.cpp†L1-L218】【F:tests/barrier_delivery_fence_repro_test.cpp†L1-L214】

## What the current delivery fence guarantees

A delivery-fence barrier call is a rendezvous barrier followed by a local call
to `Managed_process::wait_for_delivery_fence()` once the RPC to the coordinator
returns.【F:include/sintra/detail/barrier.h†L25-L110】 The coordinator resolves
the rendezvous inside `Process_group::barrier()` and returns a flush watermark
for its reply ring. Each participant flushes the coordinator's reply channel up
to that watermark before continuing.【F:include/sintra/detail/coordinator_impl.h†L32-L212】

After the rendezvous completes, every process runs `wait_for_delivery_fence()`.   
This routine snapshots the leading request/reply sequence published by each
active reader, captures weak references to the per-reader delivery-progress
structs, and waits until every captured progress counter reaches the leading
sequence. Request threads cooperate by executing deferred post-handlers and by
calling `notify_delivery_progress()` every time they advance their local read
counters.【F:include/sintra/detail/managed_process_impl.h†L1639-L1755】

## Why the guarantee is insufficient on Windows

The above protocol ensures that *local* readers have drained their inflight
traffic before a process leaves the barrier. It does **not** guarantee that
remote processes have consumed all of the caller's outgoing requests before the
coordinator releases the barrier:

1. `Process_group::barrier()` emits completion replies (and therefore flush
   tokens) as soon as the last arrival is observed. This happens inside the
   coordinator's request-handler thread, before the coordinator has had a
   chance to run its own `wait_for_delivery_fence()` on the backlog it just
   observed.【F:include/sintra/detail/coordinator_impl.h†L82-L212】
2. Workers flush the reply ring, return from the barrier, and begin emitting the
   next burst of traffic while the coordinator is still draining the backlog
   from the previous iteration. On Linux the coordinator usually catches up in
   time; on Windows the `SleepConditionVariableSRW` wakeups are sluggish enough
   that the backlog grows and the coordinator sees an apparent hole in the
   sequence stream.

The repro test highlights this race: the slow coordinator thread trusts the
barrier to cover the bursts emitted immediately before entering the fence. Once
one burst slips through—because the coordinator had not yet drained the request
ring when the last worker released the barrier—the coordinator detects the
missing sequence and aborts.【F:tests/barrier_delivery_fence_repro_test.cpp†L136-L209】

## Architectural direction

Mitigations that simply lengthen sleeps or add ad-hoc retries were previously
attempted (for example PR #605, later partially reverted) and only masked the
symptom. The issue stems from the fact that delivery-fence barriers rely on the
per-process `wait_for_delivery_fence()` helper, which only inspects local
reader progress. To make the fence robust we need an explicit handshake that
prevents the coordinator from releasing the barrier until it knows that every
participant's outbound traffic—up to the point of entering the fence—has been
observed.

One viable direction is a two-phase completion:

1. **Arrival phase:** when a process arrives at the fence it sends, along with
   the barrier request, the leading sequence of its request ring. The
   coordinator records the "drain up to" sequence for every participant.
2. **Drain phase:** once the last arrival is seen, the coordinator defers
   completion until its request reader has progressed to each recorded
   "drain-to" sequence. This drain must run outside the barrier RPC handler (for
   example via `run_after_current_handler`) so the request thread can continue
   making progress while the coordinator waits.
3. **Release phase:** only after the coordinator has drained each participant's
   request channel does it emit the completion messages and flush tokens. The
   existing per-process `wait_for_delivery_fence()` then continues to guard the
   local readers, ensuring symmetry between inbound and outbound guarantees.

Capturing the drain targets in the barrier RPC itself keeps the complexity
bounded: the information piggybacks on the existing barrier arrival message and
no global scans are required. The coordinator already tracks per-process
membership inside the barrier object, so storing the expected request sequence
alongside `processes_arrived` is a natural extension. The deferred completion
can reuse the existing `emit_barrier_completions()` path once all drains are
confirmed.

## Next steps

* Extend the barrier RPC signature so callers pass the leading request sequence
  observed just before invoking the fence.
* Persist the per-caller drain targets inside the `Process_group::Barrier`
  structure and schedule a post-handler on the coordinator to wait for those
  targets before calling `emit_barrier_completions()`.
* Teach `Managed_process::wait_for_delivery_fence()` (or a helper) to expose a
  "wait until remote has consumed sequence" primitive so the coordinator's
  post-handler can block on concrete counters rather than busy-waiting.
* Once the coordinator drains the recorded sequences and emits completions,
  keep the existing per-process delivery fence to guard local readers.
* Update the regression tests to assert that the coordinator completes every
  iteration without seeing missing markers on Windows.

## Follow-up observations (March 2025)

While attempting to prototype the "drain before release" scheme, two specific
failure modes surfaced:

* Calling `wait_for_delivery_fence()` directly from the last-arrival code path
  blocks indefinitely. The helper snapshots both request **and reply** readers;
  because the coordinator has not emitted the barrier completions yet, the
  reply-side targets can never be satisfied and the wait does not complete.
* Capturing per-process request-leading sequences after the last arrival and
  waiting for the corresponding delivery-progress counters to advance still
  hangs in practice. The coordinator's reader map includes entries for newly
  spawned workers whose request threads have not yet published any progress.
  Their delivery counters remain at zero while the leading-sequence snapshot is
  already ahead of them, so the drain wait never finishes.

The experiments confirm that a viable fix needs a way to identify only the
actively participating request readers and to exclude reply-stream targets from
the coordinator-side fence. Future work should focus on a filtered snapshot
that captures request readers for the processes listed in the barrier
completion and ignores reply readers entirely. Once that plumbing exists, the
background thread can wait on those filtered targets without deadlocking the
coordinator.

Documenting this plan should make future attempts focus on the architectural
gap instead of surface-level mitigations.
