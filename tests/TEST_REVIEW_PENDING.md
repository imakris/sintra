# Pending Test Review Follow-Ups

This document tracks remaining gaps or follow-ups identified during review of the
recently added Sintra tests. These items are not blocking fixes, but should be
considered if additional hardening or coverage is desired.

## spawn_wait_test.cpp
- Timeout-path ambiguity: the timeout test currently passes if
  `spawn_swarm_process` returns 0, but that return value also covers hard spawn
  failures (e.g., binary not found). If you want a stricter test, make the
  timeout case prove the child launched successfully (for example, have the
  timeout-mode child register a *different* name and verify that name resolves
  while the wait still times out for the nonexistent name).
- Backoff duration validation: the test now enforces a lower bound on the
  timeout duration, but does not assert an upper bound (it only warns on
  unusually long waits). If tighter enforcement is desired, consider bounding
  the upper wait time and failing when it exceeds the threshold.

## lifecycle_handler_test.cpp
- Event-to-worker correlation: the test only validates that an event of each
  reason type arrives, not that the `process_iid` belongs to the specific worker
  that triggered the condition. If you want stronger validation, include the
  worker’s process instance id (or process slot) in `Ready_signal` and verify
  that crash/unpublished/normal events correspond to the expected worker.
- Crash status strictness: the test logs crash status but does not enforce a
  non-zero value. If you want stricter behavior, add a platform-aware assertion
  (for example, accept non-zero on POSIX SIGILL and specific codes on Windows,
  while allowing 0 where the platform reports zero).

## log_stream_move_test.cpp
- Self-move expectations: the test only asserts that self-move does not crash
  and does not require specific log output. If you want deterministic behavior
  enforced, decide whether self-move should preserve or clear the stream and
  assert accordingly.

## tls_post_handler_test.cpp
- No specific pending follow-ups identified. The tests cover initialization,
  readiness, clear/release, and thread-local isolation.

## transceiver_ctor_test.cpp
- No specific pending follow-ups identified after adding resolution checks for
  named constructors.