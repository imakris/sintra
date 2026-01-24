# Stack Capture Status (Session Notes)

## Summary
- Crash-capture probes are still flaking on macOS: the latest run completed with
  20/20 passes and no crash output.
- Linux and Windows show expected failures with stack capture recorded.
- Runner output includes "rounds" because repetitions are batched (1, 2, 4, 8, ...).
  This is confusing for crash probes that should fail immediately.

## Current Behavior (as observed)
- macOS run 21323438923 (job 61376608928) completed successfully with:
  - crash_capture_child_test_debug 20/20
  - crash_capture_self_test_debug  20/20
  - No CRASH / STACK CAPTURE lines emitted.
- Linux/Windows runs (e.g., 21323438919, 21323438913) failed as expected and
  emitted "STACK CAPTURE: debugger/postmortem".
- macOS earlier run 21322354859 showed CRASH + post-mortem stack capture, but
  macOS regressed to passing on the next run.

## Recent Changes (commits on branch crash-capture-suite)
- 528cc104: Added pause window for stack capture probes, Windows child stdio
  inheritance, stack capture summary output, and other reliability tweaks.
- ea2fd6c1: Prefer macOS "sample" capture (can help stacks on macOS).
- 961c4f87: Reset stack capture history per run to allow capture each repetition.
- d298dda3: Switch probes to illegal-instruction crash for determinism.
- 948b3a6: Use SIGSEGV on macOS (attempted to restore macOS crashes).
- 93d7bed: Clarify "live stack capture unavailable" when post-mortem succeeds.
- 22de760: Force macOS probes to call abort() (attempt to fix macOS non-crash).

## What Works
- Linux and Windows crash probes fail and log stack capture results.
- When a post-mortem stack is captured after a failed live attach, the message
  now clarifies that post-mortem succeeded.

## What Does Not Work
- macOS crash probes are not consistently crashing. The latest run completed
  with no CRASH/STACK CAPTURE output, which violates the stack-capture objectives.
- The runner's "rounds" output makes repeated passes look like a test is working
  even when it never crashes.

## Should Be Fixed Next
- Make the macOS probes crash deterministically every time.
  - Investigate why abort() did not crash in run 21323438923.
  - Consider forcing a different crash path on macOS (e.g., SIGABRT via raise,
    or a direct fault) and verify in CI.
- Reduce rounds for crash-capture probes to a single repetition.
  - Either set repetitions to 1 in tests/active_tests.txt or add a runner
    override for stack-capture probes so they do not batch in rounds.
- Add a clear per-run signal in logs that a probe actually crashed (or did not),
  so silent passes are obvious.

## Notes on the Runner
- "Rounds" are produced by the repetition batching (1, 2, 4, 8, ...). The runner
  stops once a failure occurs. For crash probes, this should happen in round 1.
