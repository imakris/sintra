# Windows delivery-fence follow-up (May 2025)

## Context

The delivery-fence tests (`barrier_delivery_fence_repro_test`, `barrier_flush_test`,
and choreography variants) continue to fail intermittently on Windows. Previous
investigations identified that the coordinator releases the barrier as soon as the
last arrival is recorded, even if its request readers have not yet drained the
pre-barrier backlog. The bespoke repro test amplifies this race by making the
coordinator handler deliberately slow, which exposes missing markers after the
barrier returns.

## Attempted fix

The experiment in this session attempted to hold the barrier on the coordinator
until its local request readers observed the sequences that were in flight when
the barrier was entered. The change was limited to the coordinator and
`Managed_process`:

1. When the last arrival is observed, the coordinator defers all completions and
   schedules a post-handler (`run_after_current_handler`).
2. The post-handler collects delivery targets for every barrier participant by
   taking the coordinator's view of each request reader's leading sequence and
   waiting until that progress is reported via the existing delivery-progress
   counters.
3. Only after the wait succeeds are the barrier completions emitted on the reply
   ring.

No modifications were made to the worker-side fence logic; once the deferred
reply arrives, the existing `wait_for_delivery_fence()` continues to guard local
readers.

## Results

The approach did **not** succeed:

* The repro test now fails deterministically with
  `Coordinator did not observe first marker for iteration 1`. The coordinator
  never reaches the point where the first marker is processed, which indicates
  that barrier completions were not dispatched.
* Instrumentation added during debugging showed that the coordinator's
  `make_request_delivery_targets()` captured backlog targets for four recipients
  (presumably coordinator + three workers), but only one of the request readers
  ever advanced. The other readers reported an observed sequence of `0` even
  though their remote peers reported leading sequences around `175`, so the
  wait never completed.
* Adding additional logging confirmed that the post-handler remained blocked
  inside the delivery wait. Because completions were never emitted, the worker
  processes stayed in the barrier and the coordinator timed out waiting for the
  first iteration markers.

## Observations

* The coordinator's local view of the request sequences does not appear to match
  the sequences reported by the workers. Simply waiting for
  `reader.get_request_leading_sequence()` therefore stalls forever for some
  participants. The mismatch suggests that the coordinator may be reading a
  relayed ring or a different sequence space altogether.
* Scheduling the wait via `run_after_current_handler` succeeds; the lambda runs
  on the request thread of the last arrival. However, if any target never
  reaches the captured sequence, completions are permanently suppressed and the
  barrier fails.
* Instrumentation is essential. Future attempts should log both the remote
  sequence provided by the worker and the coordinator's local
  `get_request_leading_sequence()`/`get_request_reading_sequence()` at the moment
  the barrier request is processed. Understanding that mapping is necessary
  before implementing a reliable handshake.

## Next steps

* Investigate how the coordinator translates remote sequences into local ring
  progress. The request readers may need an explicit translation function or a
  way to expose the remote writer's leading sequence directly.
* Consider extending the barrier RPC so workers send both the leading sequence
  and the corresponding reader offset (e.g., their view of the coordinator's
  reading sequence). This would allow the coordinator to compute an absolute
  "drain-to" target without guessing the mapping.
* Alternatively, explore draining by counting messages rather than comparing raw
  sequence numbers—e.g., capture the difference between leading and reading
  sequences on the coordinator at barrier entry and wait until that backlog size
  reaches zero.

The attempted fix is not suitable for submission; all changes were discarded.
This document records the approach and the failure mode so the next iteration
can focus on the sequence-space mapping rather than repeating the same path.
