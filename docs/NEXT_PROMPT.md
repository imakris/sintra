NEXT PROMPT - BARRIER ACK REFACTOR (feature/barrier-ack-skeleton @ 23485f2c769d474c3bbcfd6026977c12f5b1079f)
================================================================================

Context Snapshot
----------------
* Coordinator now defers the final caller whenever outbound/processing guarantees are requested, launches the timeout monitor thread, and finalises barriers asynchronously once phases settle or deadlines expire.
* Added `SINTRA_TRACE_BARRIER=1` instrumentation: coordinator and participants log ack requests/responses, timeout decisions, and `wait_for_delivery_fence()` progress; the processing-fence harness writes controller/worker logs.
* Current traces show processing acks firing while the worker still reports `handler_ran=false`; `wait_for_delivery_fence()` only tracks RPC request/reply streams and misses the broadcast `world()` traffic that carries `Work_message`.

What Was Tried (and Reverted)
----------------------------
1. **Synchronous ack dispatch**: running the lambda immediately eliminated the deadlock but reintroduced the spinlock teardown crash (touched coordinator containers while the request reader still owned them).
2. **Signal-based redirect**: wiring a new Managed_process::barrier_ack_notify / Process_group::barrier_ack_signal hit Transceiver::send capability checks; participants cannot emit coordinator-owned signal types without widening sender rules. Prototype removed.
3. **Detached thread**: firing the ack from a new thread avoided the post-handler queue, but introduced coordinator lifetime hazards and still routed through the RPC reply path we want to retire.

Outstanding Work
----------------
* Extend the processing acknowledgement path so it waits on broadcast/event delivery (or another positive completion signal) instead of relying solely on `wait_for_delivery_fence`'s RPC stream tracking; `sintra_processing_fence_test_debug` remains red until this lands.
* Keep collecting telemetry with `SINTRA_TRACE_BARRIER=1`; archive controller/worker logs from failing runs for comparison after the fix.
* Stress-test teardown scenarios once the processing ack is fixed to ensure `barrier_ack_response` and the timeout monitor handle draining peers without regressions.

Suggested Next Steps (next session)
-----------------------------------
1. Rebuild: `ninja -C build-ninja sintra_processing_fence_test_debug`.
2. Run the processing fence test with `SINTRA_TRACE_BARRIER=1` and capture controller/worker logs plus stdout traces.
3. Implement the additional wait condition for event delivery, rerun the test, and verify the worker reports `handler_ran=true`.
4. Decide which tracing hooks to keep permanently and update docs/tests once the regression passes.
