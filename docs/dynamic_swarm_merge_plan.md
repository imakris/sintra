# Dynamic swarm merge plan

This plan translates the branch comparison analysis into an actionable sequence
for integrating the dynamic swarm work back into `master`.

## Goals

1. Provide deterministic dynamic-join/leave support in the runtime and
   coordinator layers.
2. Preserve developer ergonomics around branch registration and spawn helpers.
3. Keep example/test coverage focused on the rally controller flow while
   eliminating the repeated `rpc_cancelled` shutdown noise observed in current
   builds.

## Integration order

1. **Coordinator foundation**
   - Start from the coordinator RPC surface in
     `codex/implement-dynamic-process-entry-and-exit-doentf` to ensure dynamic
     spawns can be added and rolled back cleanly.
   - Backport the guarded `_sintra_*` auto-enrollment logic from
     `codex/implement-dynamic-process-entry-and-exit-dnyuom` so pre-existing or
     externally spawned processes still end up in the canonical groups without
     relying exclusively on RPC calls.
   - Reject the minimalist `codex/implement-dynamic-process-entry-and-exit`
     approach because it only covers one-way enrollment and leaves lingering
     membership state on failure.

2. **Runtime helpers**
   - Merge the branch registry and rollback-aware `spawn_branch` /
     `remove_branch` helpers from `doentf`, keeping the richer
     `Process_descriptor` metadata introduced in
     `codex/add-test-for-example-6-w2kzoc` (e.g., `auto_start`, cached instance
     ids).
   - Drop the duplicate spawn helpers from `add-test-for-example-6-w2kzoc`
     because they do not undo partial enrollments and would reintroduce the
     cleanup gaps that lead to the observed test failures.

3. **Example and tests**
   - Use the `doentf` controller choreography (spawn, ready, depart) as the
     baseline for both example 6 and the integration test. It already exercises
     entry/exit flows end to end.
   - Fold in the clearer logging from `add-test-for-example-6-w2kzoc` to aid
     deterministic debugging, but tighten shutdown sequencing so no commands are
     sent to already removed participants.
   - Retain the existing `tests/run_tests.py` runner until its rewrite can be
     reconciled with current project expectations, keeping the merge focused on
     dynamic swarm functionality.

4. **Clean-up and validation**
   - After applying the coordinator/runtime changes, replay the dynamic swarm
     integration test and example to confirm `rpc_cancelled` exceptions disappear
     or are isolated to a known bug. Capture CPU time and RSS data to confirm no
     new regressions appear.
   - Document any residual failures with reproduction steps so follow-up fixes
     can target them precisely.

## Open questions

- The `rpc_cancelled` shutdown noise might still stem from late departures or
  controller broadcast timing. If it persists after the merge, prioritize
  instrumenting the coordinator publish path to identify which subscribers are
  disconnecting unexpectedly.
- Several branches remove `tests/run_tests.py`; confirm with the broader team
  whether the file should be replaced, relocated, or left untouched to avoid
  breaking existing automation.

## Definition of done

- Dynamic swarm example and test execute without fatal `rpc_cancelled`
  exceptions in repeated runs.
- Coordinator and runtime APIs present a symmetric add/remove surface for all
  dynamic members.
- Documentation (this plan plus the branch comparison) is updated to reflect
  the merged state, and any remaining follow-ups are tracked separately.
