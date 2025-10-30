# Windows delivery-fence barrier investigation

## Summary of the observed failure

Windows CI runs intermittently fail in the delivery-fence barrier suite. The most
reproducible signal so far is the `sintra_barrier_flush_test` timeout reported in
PR #605 follow-ups and subsequent mitigation attempts:

```
Barrier flush test reported failure: Timed out waiting for iteration 10 markers. Missing workers: 0
```

The same suite occasionally times out inside `sintra_complex_choreography_test`
with the main thread parked in `sintra::detail::rendezvous_barrier` while a worker
thread is stuck in `sintra::Managed_process::flush`. Both signatures point to the
same failure mode: a participant exits `Process_group::barrier` with a flush token
that is never satisfied, so the process never leaves the barrier and all dependent
work stalls.

## Delivery-fence mechanics (code path recap)

The barrier pipeline is spread across several subsystems:

* `Process_group::barrier()` performs the rendezvous and defers all but the last
  arrival. The last arrival removes the barrier from the coordinator map and
  returns a flush watermark for its own caller. 【F:include/sintra/detail/coordinator_impl.h†L30-L120】
* Once the last caller is known, the coordinator emits completion messages for
  every waiter. Each message carries a watermark taken from
  `m_out_rep_c->get_leading_sequence()` just before the write. 【F:include/sintra/detail/coordinator_impl.h†L160-L212】
* Callers treat the returned watermark as a "reply-ring fence" and synchronously
  flush the coordinator's reply ring via `Managed_process::flush()`. The flush
  waits for the local reply reader to advance its `reply_sequence` past the
  watermark. 【F:include/sintra/detail/barrier.h†L61-L96】【F:include/sintra/detail/managed_process_impl.h†L1558-L1678】
* Reply readers publish their progress every time they consume a message and
  wake any waiter tracking a flush watermark. 【F:include/sintra/detail/process_message_reader_impl.h†L500-L620】

On paper this closes the loop: when every waiter drains the watermark, all
pre-barrier replies have been observed and the caller is free to continue.

## Suspected mismatch in sequence semantics

The investigation points to a subtle sequencing mismatch that can explain the
Windows-only timeouts:

* `m_out_rep_c->get_leading_sequence()` returns the **next** sequence number the
  coordinator will publish (`leading_sequence`). After writing a message,
  `done_writing()` stores the incremented value. 【F:include/sintra/detail/ipc_rings.h†L1674-L1785】
* The reader-side watermark that `Managed_process::flush()` polls is computed
  from `Message_ring_R::get_message_reading_sequence()`, which translates the
  internal `reading_sequence` into "sequence of the next unread element" by
  subtracting the unread span of the current snapshot. 【F:include/sintra/detail/message.h†L704-L740】

If the coordinator embeds the **pre-write** leading sequence (call it `N`) inside
the completion message, the waiter subsequently flushes until the reply reader
observes `>= N`. The reply reader, however, reports the **next unread** sequence.
After consuming the completion, the unread span becomes zero and
`get_message_reading_sequence()` evaluates to `N + 1`. That is still `>= N`, so
all platforms should succeed. The observed hang therefore implies that on
Windows the reply reader sometimes reports `N` instead of `N + 1`. In that case
waiters expect `>= N` but the reader settles on exactly `N` and never crosses the
threshold.

Two Windows-specific details may contribute to this off-by-one:

1. The reply reader lazily initialises its snapshot and `m_range` bookkeeping.
   During the short window where the completion is the only message in a new
   snapshot, the expression `reading_sequence() - (m_range.end - m_range.begin)`
   can transiently evaluate to the **last consumed** sequence instead of "next to
   consume" because `m_range` still spans the unread payload. 【F:include/sintra/detail/message.h†L704-L735】
2. `Managed_process::flush()` performs its first "fast path" check (`rs >=
   flush_sequence`) before it enqueues the watermark into `m_flush_sequence`. If
   the reader reports `N` at that moment, the flush immediately succeeds even
   though the completion body has not been processed, leaving subsequent delivery
   fence checks oblivious to the missing payload. 【F:include/sintra/detail/managed_process_impl.h†L1558-L1616】

Linux builds appear to race past the transient window quickly enough that the
flush still sees `N + 1`, which explains why the bug is Windows-dominant.

## Next steps / instrumentation ideas

To validate the hypothesis and harden the implementation we should:

1. Instrument `Process_group::emit_barrier_completions()` to log the `(recipient,
   flush_sequence)` pairs and extend `Managed_process::flush()` with debug-only
   traces showing `rs` before and after the wait. That will confirm whether the
   waiter ever sees a watermark that cannot be satisfied. 【F:include/sintra/detail/coordinator_impl.h†L160-L212】【F:include/sintra/detail/managed_process_impl.h†L1558-L1678】
2. Consider switching the completion watermark to the value returned by
   `done_writing()` (i.e. the post-write leading sequence) so every waiter flushes
   to "next sequence after the completion" instead of "sequence of the
   completion". That removes the dependency on the reader's interpretation of the
   current snapshot length.
3. Alternatively, teach `Managed_process::flush()` to treat an exact match (`rs ==
   flush_sequence`) as satisfied only after confirming that the reply handler ran.
   The handler already executes before `flush()` returns from the barrier call,
   so the additional check would only impact the delivery fence path.

Collecting the suggested traces on a Windows machine should confirm whether the
reply reader indeed stalls at the watermark. From there we can either adopt the
"post-write" watermark (preferred) or tighten the reader progress accounting so
that `>=` always reflects a consumed completion message.
