# Windows delivery-fence architecture notes (June 2025)

## Synopsis

Delivery-fence barriers still allow the coordinator to release the rendezvous before it has drained the backlog that was present when the last participant arrived. The bespoke repro (`tests/barrier_delivery_fence_repro_test.cpp`) keeps the coordinator request handler artificially slow and reliably observes missing iteration markers whenever the coordinator skips over one of the bursts that should have been flushed by the fence.【F:tests/barrier_delivery_fence_repro_test.cpp†L1-L209】 The failure rate is dramatically higher on Windows because `SleepConditionVariableSRW` wake-ups let request readers fall behind for long stretches.

The current implementation acknowledges the rendezvous and immediately returns a reply-ring watermark. Each worker flushes the coordinator's reply ring and then relies on `Managed_process::wait_for_delivery_fence()` to ensure its *local* readers have drained their streams before continuing.【F:include/sintra/detail/coordinator_impl.h†L31-L212】【F:include/sintra/detail/managed_process_impl.h†L1642-L1715】 None of these steps makes the coordinator wait for the backlog it just observed, so the workers can emit the next burst while the coordinator is still catching up, triggering the Windows-only gaps documented earlier.

## Ground truth about the sequence counters

While reading through the code it is easy to lose track of which sequence number represents what. The relevant facts are:

* The request reader thread updates `Delivery_progress::request_sequence` with the value returned by `Message_ring_R::get_message_reading_sequence()` every time it consumes an element.【F:include/sintra/detail/process_message_reader_impl.h†L220-L320】 That value is the sequence of the next unread element from the coordinator's point of view.【F:include/sintra/detail/message.h†L688-L742】
* `Process_message_reader::prepare_delivery_target()` already encapsulates the "wait until `>= target`" logic by combining the captured sequence with the delivery-progress weak pointer.【F:include/sintra/detail/process_message_reader_impl.h†L360-L450】
* `Managed_process::wait_for_delivery_fence()` skips readers that are still in `READER_SERVICE` or `READER_STOPPING`, so any handshake that reuses these helpers will automatically ignore rings that are not yet publishing progress.【F:include/sintra/detail/managed_process_impl.h†L1647-L1687】

These details mean we do not need a new sequence space or an out-of-band translation: if the coordinator captures the request-reader progress via the existing helpers, the resulting targets will line up with the waiters' view.

## Proposed two-phase completion

To close the gap without exploding complexity, the barrier needs an explicit drain phase. A workable approach is:

1. **Augment the barrier RPC.** Before calling `Process_group::barrier()`, the worker should capture the sequence of its outgoing request ring ("drain everything ≤ this sequence") and include it in the RPC payload. `Transceiver::rpc_impl` already serialises barrier arguments, so adding a `sequence_counter_type` parameter keeps the change local.【F:include/sintra/detail/barrier.h†L27-L88】
2. **Record drain targets on the coordinator.** When the last arrival is observed, store, per participant, the pair `(request_reader*, target_sequence)` and move the completion logic into a post-handler scheduled with `Managed_process::run_after_current_handler()` so that the request thread keeps ownership of the reply ring while waiting.【F:include/sintra/detail/coordinator_impl.h†L70-L212】【F:include/sintra/detail/managed_process_impl.h†L1720-L1755】
3. **Leverage the existing delivery fence helper.** Inside the post-handler, call a new helper that accepts the recorded `Delivery_target` objects and waits until every target reports progress `>= target_sequence`. The helper can be a thin wrapper around `Managed_process::wait_for_delivery_fence()` that skips the snapshot step and directly uses the supplied targets.
4. **Emit completions once the drain succeeds.** When all targets are satisfied, emit the barrier completions and return the per-recipient reply watermark exactly as today.【F:include/sintra/detail/coordinator_impl.h†L176-L212】 Workers will subsequently run their usual local delivery fence, preserving the existing guarantees.

This design keeps the single-writer invariant (the request thread remains the sole publisher on the reply ring) and avoids detached helper threads, addressing the pitfalls listed in the May follow-up document.【F:docs/windows_delivery_fence_followup.md†L11-L105】 Because the coordinator records explicit drain targets, it cannot release the barrier until the backlog the workers had already sent has been observed.

## Open questions / instrumentation hooks

* The barrier message today contains only the barrier name. We need to audit the call sites in `Transceiver::rpc_impl` and the generated message layouts to ensure we can carry the additional `sequence_counter_type` without ABI drift for other barrier modes.
* Startup races remain possible if some request readers are still initialising (state ≠ `READER_NORMAL`). One option is to have workers delay the first delivery-fence barrier until every `Process_message_reader` reports `READER_NORMAL`; another is to teach the coordinator to treat `invalid_sequence` targets as "no drain required".
* We should add tracing around the post-handler (`[barrier] recorded drain target=... observed=...`) so that future Windows runs can confirm the coordinator really waited for the backlog before releasing the barrier.

## Recommended next steps

1. Extend the barrier RPC and regenerate the helper templates so that workers can ship their request-ring leading sequence alongside the barrier name.
2. Store the `(reader, target_sequence)` pairs on the coordinator when `processes_pending` becomes empty, then schedule a post-handler that waits for those targets.
3. Implement a `Managed_process::wait_for_delivery_targets(span<Delivery_target>)` helper that shares the waiting loop with `wait_for_delivery_fence()` but operates on a caller-provided snapshot.
4. Add debug-only counters in the repro test to assert that the coordinator never observes missing markers once the two-phase completion is in place.

These changes keep the architectural complexity contained while finally giving the coordinator an explicit opportunity to drain outstanding request traffic before releasing a delivery-fence barrier.
