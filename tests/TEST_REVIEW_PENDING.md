# Pending Test Review Follow-Ups

This document tracks remaining gaps or follow-ups identified during review of the
recently added Sintra tests. These items are not blocking fixes, but should be
considered if additional hardening or coverage is desired.

## spawn_wait_test.cpp
- ~~Timeout-path ambiguity: the timeout test currently passes if
  `spawn_swarm_process` returns 0, but that return value also covers hard spawn
  failures (e.g., binary not found).~~ **RESOLVED**: The timeout-mode child now
  registers `kTimeoutChildInstanceName` and the coordinator verifies it resolves
  after the timeout, proving the child launched successfully. If the child never
  launched, this verification fails the test.
- Backoff duration validation: the test now enforces a lower bound on the
  timeout duration, but does not assert an upper bound (it only warns on
  unusually long waits). **DEFERRED**: An upper bound would make the test fragile
  on slow or loaded CI systems. The warn-but-don't-fail approach is appropriate.

## lifecycle_handler_test.cpp
- ~~Event-to-worker correlation: the test only validates that an event of each
  reason type arrives, not that the `process_iid` belongs to the specific worker
  that triggered the condition.~~ **RESOLVED**: `Ready_signal` now includes
  `process_iid`, workers send their process instance ID, and the coordinator
  tracks a `worker_id -> process_iid` mapping. Each lifecycle event is validated
  to ensure the `process_iid` matches the expected worker that triggered it.
- Crash status strictness: the test logs crash status but does not enforce a
  non-zero value. **DEFERRED**: Platform differences in signal/exit status
  reporting make strict assertions fragile. The current logging approach allows
  manual verification while avoiding false failures.

## log_stream_move_test.cpp
- Self-move expectations: the test only asserts that self-move does not crash
  and does not require specific log output. **DEFERRED**: Per C++ standard,
  self-move-assignment leaves the object in a "valid but unspecified state".
  The test correctly validates safety (no crash) without mandating specific
  behavior, which matches the standard's guidance.

## tls_post_handler_test.cpp
- No specific pending follow-ups identified. The tests cover initialization,
  readiness, clear/release, and thread-local isolation.

## transceiver_ctor_test.cpp
- No specific pending follow-ups identified after adding resolution checks for
  named constructors.
