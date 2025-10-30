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

## Additional observations (April 2025)

During a follow-up investigation we attempted to prototype the two-phase
handshake outlined above. The proof-of-concept extended the barrier RPC so the
coordinator captured each caller's request-ring leading sequence and queued a
background wait using `Managed_process::wait_for_delivery_targets()`. While the
approach looked promising on paper, running
`sintra_barrier_delivery_fence_repro_test_release` on Linux highlighted two
practical issues:

* Startup barriers execute before every `Process_message_reader` has fully
  transitioned to `READER_NORMAL`. Capturing drain targets for readers still in
  the `READER_SERVICE` state yielded absolute sequence values around `315`
  (matching the ring's persistent leading sequence) while the corresponding
  progress counters remained at `0`. Because the request threads have not begun
  draining those rings yet, the background wait never completes and the barrier
  replies never fire.
* Even once the readers become active, scheduling the wait on a detached thread
  introduces ordering hazards. Multiple threads attempt to publish barrier
  completions onto the reply ring, violating the single-writer expectation and
  risking deadlocks when the request thread still owns the ring's writer slot.

The debugging logs below illustrate the stall. The coordinator recorded a drain
target of `315` for each worker while the observed progress remained `0`:

```
[barrier] /include/sintra/detail/managed_process_impl.h:1328 waiting on 3 remote targets
[delivery] checking 3 targets
  target=315 observed=0 stream=req
```

Eventually, once the request threads advanced the progress counters, the drain
would complete, but the lengthy stall made the test impractically slow and
susceptible to timing out:

```
[delivery] checking 3 targets
  target=315 observed=315 stream=req
[barrier] remote drain satisfied
[barrier] completions emitted
```

Future work should avoid relying on detached threads and instead piggyback on
the request loop itself. One option is to queue a lightweight post-handler via
`run_after_current_handler()` that re-checks the recorded delivery targets after
each message and only emits completions once all targets are satisfied. This
keeps the single-writer invariant intact and sidesteps the startup edge case by
skipping readers that have not entered the normal processing state yet.

Documenting this plan should make future attempts focus on the architectural
gap instead of surface-level mitigations.

## October 2025 status update

Two mitigation experiments were attempted but neither yielded a viable fix:

1. **Post-handler driven drain.** The change captured per-process request-ring
   leading sequences and queued a post-handler via
   `Managed_process::run_after_current_handler()`. The idea was to re-check the
   recorded targets after each request handler finished. In practice the
   handler executed only once—immediately after the barrier RPC returned—because
   the coordinator's request loop had no further messages to trigger additional
   post-handler runs. As a result the recorded targets were never re-evaluated
   and the barrier hung, leaving the repro test stuck.
2. **Synchronous `wait_for_delivery_fence()` call.** Calling
   `Managed_process::wait_for_delivery_fence()` directly inside
   `Process_group::barrier()` (just before emitting completions) causes the
   coordinator's request thread to block while holding the barrier. Remote
   readers cannot make progress under that lock, so the wait never completes and
   the barrier deadlocks.

Both experiments reinforce the need for a proper two-phase completion that ties
into the coordinator's request loop without blocking it. Any future attempt
should supply a mechanism for re-evaluating pending drain targets when other
request reader threads publish progress (for example by storing the targets in a
shared structure that `notify_delivery_progress()` can service) rather than
relying on the current request handler to poll.
