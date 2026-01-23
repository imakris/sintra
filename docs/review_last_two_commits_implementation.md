# Implementation Notes: Review Report Follow-up

Source: `docs/review_last_two_commits.md`

## Changes implemented
- Made stale-guard delay configurable via `SINTRA_STALE_GUARD_DELAY_MS`.
- Hardened stale-guard clearing to reset the timer when a real blocking reader
  is observed and when `read_access` changes, and to avoid double-waiting on
  already-queued signals in the Windows dispatch path.
- Skipped the Windows SEH wait in the vectored handler to reduce duplicate
  blocking in crash paths.
- Added `test_stale_guard_clears_after_timeout` in
  `tests/ipc_rings_tests.cpp` with a shorter stale-guard delay for faster tests.

## Status by finding
1) Potential data overwrite due to stale-guard clearing while a reader is live
   but delayed before updating its guard token.
   - Status: Partially mitigated.
   - Notes: Timer now resets on real blockers and changing access state, but a
     reader stalled for the full delay before setting the guard token could
     still be cleared as stale.

2) Stale-guard timer not reset when a real blocking reader appears.
   - Status: Addressed.
   - Notes: Timer resets when guard tokens are observed.

3) Duplicate crash notification path on Windows (VEH + CRT signal path).
   - Status: Partially mitigated.
   - Notes: `queue_signal_dispatch_win` now skips waiting when a signal is
     already pending, and the VEH path skips waiting entirely. This reduces
     duplicate waiting but does not fully prevent duplicate dispatch if the
     signal handler runs after the first dispatch completes.

4) Missing SEH mappings compared to debug-pause handler.
   - Status: Not addressed.
   - Notes: No change to exception mapping set.

5) No cleanup for vectored exception handler registration.
   - Status: Not addressed.

6) No test coverage for stale-guard clearing path.
   - Status: Addressed.
   - Notes: Added a focused unit test.
