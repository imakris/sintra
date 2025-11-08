# Windows delivery-fence investigation (August 2025)

## Objective

Prototype a coordinator-side delivery fence that holds `Process_group::barrier()` until the
coordinator drains the request backlog contributed by every participant. The goal was to make
`barrier_delivery_fence_repro_test` pass deterministically on Windows by deferring the
barrier reply until the coordinator confirms that each worker's request ring has been
processed up to the watermark recorded at barrier entry.

## Implementation sketch

* Added a `Managed_process::wait_for_remote_delivery(process_ids)` helper that mirrors the
  waiting loop in `wait_for_delivery_fence()` but scopes the targets to a specific set of
  processes.
* Taught `Process_group::barrier()` to capture the participating process IDs when the last
  arrival is observed, schedule a post-handler via
  `Managed_process::run_after_current_handler()`, and convert the barrier reply into a
  deferred completion emitted from that post-handler.
* Inside the post-handler, collected `Delivery_target` entries for each participant and
  called `wait_for_remote_delivery()` before emitting completions with
  `emit_barrier_completions()`.

## Observations

Extensive instrumentation was added around the new helper to trace the ring sequences and
`Delivery_progress` counters observed at barrier entry. The relevant logs showed two distinct
failure modes:

1. **Startup epoch mismatch.** On the initial barrier iterations every participant reported
   `req_target == req_read == 315` while the corresponding `Delivery_progress::request_sequence`
   remained at `0`. The coordinator therefore waited on an unreachable watermark because the
   delivery progress was still at its default value even though the ring reader had already
   advanced.
2. **Stale progress counters mid-run.** After skipping dormant readers the repro test still
   stalled with entries such as `req_target == req_read == 1546` while the delivery progress
   plateaued at `1362`. No new backlog was present-the coordinator ring reader had already
   caught up-but the captured progress counters never reflected the updated read position, so
   the wait loop blocked indefinitely and barrier completions were never emitted.

The repro test continued to fail with `Coordinator did not observe first marker for iteration 1`
when these waits were enabled, and it also failed when the waits were skipped because the
coordinator still released the barrier too early.

## Conclusions

* Relying on raw `Delivery_progress::request_sequence` values is brittle. The counters lag
  behind the ring reader both during startup (zero-initialised epoch) and mid-run (lagging by
  ~180 sequences) even when the ring has been fully drained.
* `req_target == req_read` is a necessary but insufficient condition: it confirms the ring is
  empty but does not guarantee the delivery-progress observers have published that fact.
* Any future handshake must normalise sequence epochs or derive backlog sizes from values
  that advance synchronously with the request reader (e.g., deltas between `reader.get_*`
  snapshots) rather than comparing absolute `Delivery_progress` counters to ring-leading
  sequence numbers.
* Deferring the barrier reply via `run_after_current_handler()` is promising, but the wait
  predicate needs to be expressed in terms that do not depend on delivery-progress counters
  catching up on their own.

The instrumentation code was reverted; only this documentation captures the findings so the
next iteration can focus on robust backlog measurements instead of replaying the same path.
