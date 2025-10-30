# Delivery fence follow-up notes (2025-03-18)

## Background

* Windows barrier timeouts originate from the coordinator releasing the delivery-fence before draining the request backlog produced immediately before the barrier call. The bespoke `barrier_delivery_fence_repro_test` exposes the race: the coordinator sleeps while workers issue bursts, then checks for missing markers once the barrier completes.
* The existing pipeline implements `Process_group::barrier()` as a rendezvous that replies as soon as the last arrival is observed. Each participant then calls `Managed_process::wait_for_delivery_fence()` to flush its **local** readers. There is no guard that ensures the **remote** coordinator has consumed the caller's pre-barrier traffic.
* `docs/windows_delivery_fence_notes.md` already suggests a two-phase completion protocol: record each participant's request sequence on arrival, then hold the barrier until the coordinator drains every recorded sequence.

## Investigation summary

* Implemented several experiments attempting to introduce the "drain phase" outlined above. The central idea was to extend `Process_group::Barrier` with per-participant drain targets and postpone barrier completions until those targets are observed.
* Two integration strategies were attempted:
  1. **Blocking post-handler** – schedule a `run_after_current_handler` continuation that directly waits on `Managed_process::wait_for_delivery_targets(...)`. This deadlocked the request reader: draining requires the reader to continue processing messages, but the wait ran on the same thread.
  2. **Deferred completion queue** – capture `Delivery_target` snapshots per participant, stash them in a pending list, and poll them from `process_pending_barrier_completions()` whenever delivery progress is announced. Although this avoided blocking the request loop, the repro test still stalled, indicating that the coordinator never observed all targets.
* Instrumentation confirmed that the targets are computed correctly (they correspond to the leading request sequence at barrier entry), but coordinating their fulfilment without starving the request loop remains tricky. In particular, joining the wait loop with the request thread leads to the same starvation scenario described above.

## Proposed direction

The outstanding work is still aligned with the plan documented in `windows_delivery_fence_notes.md`, but needs additional infrastructure to avoid stalling the request loop:

1. Introduce a dedicated progress watcher on the coordinator side instead of running the wait inside the request thread. This watcher could be a lightweight background task fed by `Managed_process::notify_delivery_progress()` events. It would periodically scan the pending drain targets and emit barrier completions once every target has advanced past its recorded sequence.
2. Ensure that the watcher gracefully handles participants disappearing mid-barrier (e.g. process draining) by clearing the associated targets and emitting a failure completion.
3. Once the watcher emits completions, the existing `Managed_process::wait_for_delivery_fence()` continues to guard local readers, preserving the symmetry between inbound and outbound guarantees.
4. Re-run `barrier_delivery_fence_repro_test` with the new watcher to confirm that the coordinator no longer misses iteration markers on Windows.

## Next steps

* Prototype a coordinator-local progress watcher that consumes a queue of `Delivery_target`s and drives completion emission without blocking the request loop.
* Extend the regression test with additional logging so that future iterations can assert that every iteration's drain targets were satisfied before the barrier released.
* Once the watcher stabilises, re-enable the repro test on Windows CI and capture the before/after timing data.

