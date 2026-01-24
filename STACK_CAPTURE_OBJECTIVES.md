# Stack Capture Objectives

## Purpose
This task exists to harden stack capture for unexpected crashes in real tests.
Crashes are failures; the test system must still collect reliable stacks for
postmortem analysis across all CI platforms.

## Scope
- Target platforms: macOS, Linux, FreeBSD (Cirrus), Windows (GitHub Actions).
- Focus on the test runner and debugger integration that capture stacks when a
  test crashes unexpectedly.

## Critical Objectives
- A crashing test must remain a failure. Stack capture is diagnostic, not a
  success condition.
- Stack capture should be predictable and reliable on all supported systems.
- The two crash-capture tests are probes of the capture pipeline only:
  - crash_capture_self_test_debug
  - crash_capture_child_test_debug
  These are not "real" tests, and their purpose is to validate that stack
  capture works end-to-end.

## Non-Goals
- Do not treat crash-capture tests as passing because a stack was captured.
- Do not change the pass/fail semantics of non-crash tests.
- Do not optimize for passing CI at the expense of missing or unreliable
  stack capture.

## Working Assumptions
- When any test crashes, the runner should attempt live and/or postmortem stack
  capture and include that output in the failure diagnostics.
- The crash-capture probes should crash deterministically to exercise the
  capture path, but still report failure like any other crash.