# Follow-up task prompt template

**Context**: Delivery-fence barriers on Windows still fail because the coordinator releases the barrier before draining its request backlog. See `docs/windows_delivery_fence_strategy.md` for the current architectural plan.【F:docs/windows_delivery_fence_strategy.md†L1-L88】 The bespoke regression test (`tests/barrier_delivery_fence_repro_test.cpp`) reproduces the failure by slowing down the coordinator handler.【F:tests/barrier_delivery_fence_repro_test.cpp†L1-L209】 The goal of the follow-up task is to implement the two-phase completion described in the strategy doc and make the repro test pass on Windows without timeouts.

**Suggested steps for the assignee**:

1. Extend the barrier RPC so callers pass their request-ring leading sequence alongside the barrier name. Touch `include/sintra/detail/barrier.h` and the generated RPC plumbing in `include/sintra/detail/transceiver_impl.h`.
2. Update `Process_group::barrier()` (`include/sintra/detail/coordinator_impl.h`) to store drain targets when the last arrival is observed and schedule a post-handler via `Managed_process::run_after_current_handler()`.
3. Implement a `Managed_process::wait_for_delivery_targets()` helper that consumes a list of `Process_message_reader::Delivery_target` objects and reuses the existing waiting loop from `wait_for_delivery_fence()`.
4. Emit barrier completions only after the post-handler confirms the coordinator's request readers have reached every recorded target.
5. Run `barrier_delivery_fence_repro_test` and the barrier suites on Windows to confirm the regression is fixed.

**Acceptance criteria**:

* `barrier_delivery_fence_repro_test` must fail on the current main branch but pass reliably after the change.
* No new helper threads are introduced; all waits must remain on the request thread via post-handlers.
* Add instrumentation or unit coverage that demonstrates the coordinator waited for the recorded drain targets.

**Additional notes (August 2025):**

* Instrumentation from the most recent attempt (`docs/windows_delivery_fence_aug2025_attempt.md`) showed that
  `Delivery_progress::request_sequence` often lags far behind the ring reader. Startup barriers reported
  `req_target == req_read == 315` while `request_sequence == 0`, and later iterations showed
  `req_target == req_read == 1546` with `request_sequence == 1362`. Waiting for the raw watermark therefore
  never completed even though the coordinator had already drained the ring.
* When implementing the helper mentioned in step 3, compute per-reader backlog deltas using the ring snapshots
  (e.g., `reader.get_request_leading_sequence()` minus `reader.get_request_reading_sequence()`) and wait for the
  delivery progress to advance by that delta rather than comparing absolute sequence numbers.
* Make sure the coordinator ignores readers whose delivery progress has never advanced (still zero) until they
  publish their first update, otherwise the post-handler will block indefinitely on dormant readers.
