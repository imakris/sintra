NEXT PROMPT – BARRIER ACK REFACTOR (feature/barrier-ack-skeleton @ 23485f2c769d474c3bbcfd6026977c12f5b1079f)
================================================================================

Context Snapshot
----------------
* sintra_processing_fence_test_debug still times out (40 s) on Windows MinGW builds: python tests/run_tests.py --test sintra_processing_fence_test --timeout 40 --build-dir ../build-ninja --config Debug --verbose.
* Coordinator remains blocked in Process_group::barrier, waiting on completion_cv because no processing acks are ever recorded.
* Current behaviour documented in docs/barrier_processing_deadlock_analysis.md — participant acks are queued via un_after_current_handler, so they never run while the barrier RPC is active.

What Was Tried (and Reverted)
----------------------------
1. **Synchronous ack dispatch**: running the lambda immediately eliminates the deadlock but reintroduces the spinlock teardown crash (touches coordinator containers while request reader still owns them).
2. **Signal-based redirect**: wiring a new Managed_process::barrier_ack_notify / Process_group::barrier_ack_signal hit Transceiver::send capability checks; participants cannot emit coordinator-owned signal types without widening sender rules. Prototype removed.
3. **Detached thread**: firing the ack from a new thread avoids the post-handler queue, but introduces coordinator lifetime hazards and still routes through the RPC reply path we want to retire.

Outstanding Work
----------------
* Design a one-way acknowledgement channel (likely a coordinator-handled signal) that participants are allowed to emit without relying on the pending RPC backlog.
* Ensure the coordinator side (Process_group::barrier_ack_response or equivalent signal handler) drains processing_waiters and toggles waiting_processing safely during teardown.
* Re-run sintra_processing_fence_test and other barrier suites once the new path lands; update docs/tests accordingly.

Suggested Next Steps (next session)
-----------------------------------
1. Rebuild: 
inja -C build-ninja sintra_processing_fence_test_debug.
2. Reproduce hang to confirm baseline.
3. Implement the new acknowledgement transport (signal or asynchronous coordinator queue) that does not depend on un_after_current_handler.
4. Verify coordinator state updates and teardown safety; extend docs/barrier_processing_deadlock_analysis.md with the chosen fix.
