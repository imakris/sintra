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
