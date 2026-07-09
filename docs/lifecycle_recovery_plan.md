# Lifecycle Recovery Plan

Status: baseline selected. Run one architecture/scope review for Slice 1, then
implement from a clean `02f03bf` worktree.

## Relation To Existing Lifecycle Plans

This document is the canonical plan for recovering the current lifecycle
refactor. Older lifecycle planning docs remain evidence and design references,
not competing instructions.

- `docs/STALE_lifecycle_group_publication_rebuild_plan.md` remains the reference for
  the B1/B2 group-publication investigation. This recovery plan owns the stop
  condition: Slice 6 may take only membership-authority work. If publication or
  rollback ownership is required, stop and reopen the B2 design separately.
- The older lifecycle contract and remediation docs are historical context for
  why broad blocker matrices, catch-all lifecycle gates, and result scaffolding
  are not being carried forward wholesale.
- Record the baseline decision and future amendments in this file, not in
  ignored scratch files or chat.

## Goal

Recover the useful lifecycle fixes without carrying forward the redesign
scaffolding that expanded state space, narrowed CI confidence, and mixed
unrelated failure modes.

## Preserve First

Before any cleanup:

- Preserve `origin/lifecycle-group-publication-rebuild` at `64dc45f`.
- Preserve local `lifecycle-group-publication-rebuild` at `1c6773d`.
- Preserve the dirty working tree as a WIP patch or WIP branch. It is not the
  same thing as `64dc45f`; it includes a divergent hard-dead retirement rewrite
  and a new `tests/hard_dead_process_retirement_test.cpp`.
- Keep these references readable: `origin/lifecycle-contracts-redesign` at
  `67d56c4`, `lifecycle-contracts-redesign-repair` at `1cf7239`, and
  `origin/master`/`master` at `02f03bf`.

Do not discard the dirty tree after tagging only the remote branch. Record the
refs, dirty status, baseline decision, review outputs, and gate results in this
file or existing review/CI artifacts. Chat history is not the control plane, and
this plan must not spawn extra control-plane documents.

Preservation record:

- Timestamp: `20260709_022406`.
- Live refs: `refs/preserve/lifecycle-recovery/20260709_022406/*`.
- Artifact directory:
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\recovery-preserve-20260709_022406`.
- Preserved refs bundle: `preserved_refs.bundle`, verified with `git bundle verify`.
- Dirty tree artifacts: `staged_docs.patch`, `unstaged_tracked.patch`,
  `all_tracked_from_head.patch`, `git_status_short.txt`, `untracked_files.txt`.
- Untracked build tree `bv/` is archived as `untracked_bv.zip`.

## Topology

The history is a fork, not one timeline, and the local repair branch is not at
the remote tip.

- `02f03bf` is the clean base under discussion.
- `02f03bf` is an ancestor of `9b170e7`.
- `9b170e7` is the common ancestor of the two lifecycle lines.
- `67d56c4` is the old admission-redesign sibling. It contains admission slice
  work through direct-spawn gating, but not RPC 4A/4B, 6A0 drain, group
  membership, or hard-dead retirement.
- `436e7f2`, `1cf7239`, `1c6773d`, and `64dc45f` are on the repair/publication
  sibling. They do not inherit `67d56c4`; similar admission ideas were
  reimplemented there.
- Local `1c6773d` is seven commits behind
  `origin/lifecycle-group-publication-rebuild`:
  `2cbbb1c -> f6dac12 -> 330a29f -> bf3ba2e -> 5cd9149 -> edd32dc -> 64dc45f`.
  These commits are salvage sources on the remote tip, not local-HEAD history.

Therefore no plan may say "start at `67d56c4` and keep later repair commits"
without explicitly cross-grafting from the sibling branch.

Pinned CI fact: `67d56c4` passed FreeBSD, Linux, macOS, and Windows Actions on
2026-07-04. Treat that as evidence for that sibling only, not as proof of the
repair/publication line.

## Baseline Audit

Do not choose the baseline by commit label. Audit each candidate as a different
composition of subsystems, then record the chosen baseline in this file.

For each candidate, inventory what is present, what must be deleted, and what
must be cross-grafted:

- `02f03bf`: clean base. Candidate if the old code only needs the external
  claim hole plus independent fixes. No admission gate, RPC terminal enum, 6A0
  drain, group membership authority, or hard-dead retirement is inherited.
- `9b170e7`: actual fork point. Candidate if the audit wants shared lifecycle
  scaffolding after `02f03bf` but before either sibling's admission/repair work.
  It still inherits no `67d56c4` admission line and no remote-tip salvage
  commits.
- `67d56c4`: old admission-redesign sibling. Candidate only if the audit fixes
  any join/recovery close-after-check regression before other work. It still
  needs every accepted repair-line fix cross-grafted and must reconcile the
  independently reimplemented admission line on the repair/publication sibling.
- `436e7f2`: repair sibling through 4A. Candidate only if the audit explicitly
  keeps or deletes its inherited runtime-work counters, pending-start/recovery
  owners, and `Finalize_blocker_kind` matrix. Do not select it merely because
  it has useful RPC terminal-state work.
- `1cf7239`: repair sibling through 6A0. Candidate only if the audit proves the
  4B lifecycle expansion is removed or independently safe. Otherwise quarry
  only the narrow 6A0 idea.

Baseline choice rule: pick the smallest candidate that keeps real fixes and
does not require preserving speculative lifecycle state.

Known quarry locations:

- `288139f` and `d373c56` are on the repair/publication sibling and
  `origin/lifecycle-contracts-redesign-repair`.
- `9473218` is on the repair/publication sibling and
  `origin/lifecycle-group-publication-rebuild`.

The audit should verify branch location for any additional quarry commit before
using it.

Baseline decision record, 2026-07-09:

- Current control branch: `lifecycle-group-publication-rebuild` at `f0aff64`,
  with the pre-existing dirty tree preserved in
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\recovery-preserve-20260709_022406`.
- Six independent xhigh audit lanes agreed to use `02f03bf` as the rebuild
  baseline. No lane recommended `67d56c4`, `436e7f2`, `1cf7239`, local
  `1c6773d`, or remote `64dc45f` as the implementation baseline.
- Reason: `02f03bf` has the smallest viable lifecycle state machine. The real
  first bug is narrow: `claim_external_process_invitation()` does not exclude
  teardown the way invitation creation and other admission-sensitive paths do.
  Later branches contain useful quarry, but they also carry speculative blocker
  matrices, runtime-work counters, pending-owner state, drain snapshots, and
  white-box tests that are not baseline requirements.
- `67d56c4` remains quarry for external invitation claim/rollback shape and
  direct-spawn gating ideas. It is not a baseline because it is a sibling of the
  repair/publication line and still needs join/recovery admission repair before
  it can be trusted.
- `436e7f2` and `1cf7239` remain quarry for 4A RPC terminal cleanup and the
  narrow 6A0 drain idea. They are not baselines because the deletion burden is
  larger than cross-grafting the useful pieces.
- First implementation slice: fix only the external invitation claim admission
  hole on `02f03bf`, with one focused regression test, then run focused local
  checks before the six-reviewer and CI gates.

Lifecycle test classification for this baseline:

- Keep existing `02f03bf` public-behavior tests:
  `external_process_invitation_test`,
  `external_process_invitation_lifecycle_negative_test`,
  `external_process_invitation_rejection_cleanup_test`,
  `external_process_invitation_admission_negative_test`,
  `finalize_emit_rpc_test`, `finalize_async_rpc_lifecycle_test`,
  `barrier_drain_and_unpublish_test`, `drain_wake_coordination_test`, and
  `choreography_extreme_test`.
- Add or adapt one focused external invitation claim test for the Slice 1 bug.
- Quarry only, do not carry as gates for Slice 1:
  `admission_boundary_contract_test`,
  `admission_live_entry_runtime_ownership_test`,
  `admission_process_entry_contract_test`,
  `admission_raw_latch_cutover_contract_test`,
  `admission_rpc_registration_contract_test`,
  `admission_spawn_recovery_contract_test`, and
  `finalize_result_contract_test`.
- Conditional later-slice quarry:
  `finalize_closed_admission_teardown_rpc_contract_test` for Slice 4,
  `shutdown_user_barrier_departure_test` for Slice 5,
  `process_group_membership_authority_test` for Slice 6, and
  `hard_dead_process_retirement_test` for Slice 7.
- CI/test discipline finding: the current branch builds 91 top-level tests and
  has 90 active entries; `barrier_delivery_fence_repro_test` is the current
  built-but-not-active test. Do not claim a final full-suite gate until built
  tests and active-test execution are reconciled or every exclusion is recorded
  with a reason.

## Implementation Slices

Each slice has one objective, one focused test or existing test target, and no
speculative public/test helper surface. If a slice needs broad new state, stop
and split it.

For every conditional slice, record a durable yes/no decision in this file
before skipping it: whether the selected baseline exhibits the bug, what
evidence was checked, and which focused gate covers the decision.

### Slice 0: CI And Test Discipline

- Keep focused CI for development: a recently-failed test set is allowed while
  the branch is red and full CI costs hours.
- `bf3ba2e` is CI/test-selection only. Keep the CMake focus mechanism only for
  local/manual focus runs if useful; do not keep its workflow activation or
  reduced `active_tests.txt` roster as the normal branch signal.
- Before claiming green or merging, the normal CI must compile and run the full
  configured suite on Linux, macOS, Windows, and FreeBSD.
- "Full configured suite" means no built test binary is silently excluded by the
  `active_tests.txt` allow-list. If a merge gate still uses an allow-list, every
  excluded built test must be named with a reason.
- Triage `ee15fec` and the dirty edits to `tests/active_tests.txt`,
  `tests/runner/configuration.py`, and `tests/external_process_invitation_test.cpp`
  here or in the admission slice. Do not leave them as unowned cleanup.

### Slice 1: Independent Base Fixes

Reapply only fixes orthogonal to the lifecycle redesign:

- Unconditionally fix the original external invitation/claim hole on the chosen
  baseline. If the baseline has no admission gate, this is the narrow existing
  claim path fix; if it keeps an admission gate, Slice 2 must prove this same
  hole is covered there.
- `a29dfbc`: static teardown registry lifetime and `deactivate_all` safety.
- `5cd9149`: process liveness/zombie handling, robust mutex PID/TID reuse, and
  lifeline release after local message-ring attachments are dropped.
- `64dc45f`: choreography child-shutdown failure propagation in tests.

For `2cbbb1c`, keep only independent fragments here if they apply cleanly:
ring writer-owner-lost detection and reader `shared_from_this` lifetime. The
retirement subsystem belongs to Slice 7.

### Slice 2: Minimal Admission Repair

Only if the selected baseline keeps an admission gate:

- Verify the unconditional external claim fix from Slice 1 is represented in
  the gate.
- Preserve atomic check-and-commit for `join_swarm`, recovery, and direct spawn.
- Quarry from `288139f` only minimal admission close/claim/commit mechanics if
  needed.
- Do not replay the full 4B runtime-work/finalize-blocker/public-RPC gate.
- Do not add runtime-work counters, pending-start owners, or lifecycle-state
  exchange APIs unless a focused failing repro proves they are necessary.

### Slice 3: RPC Terminal Cleanup

- Keep the 4A-style collapse from overlapping RPC cleanup booleans into one
  terminal state enum if it remains self-contained.
- `d373c56` is allowed only after extracting the minimal
  `cleanup_rpc_handle_if_terminal_blocking` helper from its repair-line
  dependencies. If that helper cannot stay small, drop `d373c56`.
- Do not keep the broad `Finalize_blocker_kind` matrix. Add a blocker kind only
  when production code emits and consumes it.

### Slice 4: Finalize And Drain

Only if a focused repro or audit proves the issue on the selected baseline:

- From `5062001`, salvage `s_finalize_drain_prepared` as distinct from
  "admission closed" and explicit teardown drain registration after admission is
  closed.
- From `5062001`, salvage external-attached-process exclusion from drain-wait
  candidates if external attachment remains in the selected baseline.
- For 6A0, reimplement only the small rule: drain wait completes on known
  non-self cleanup/removal, not merely on "draining".
- Do not take the 6A0 cleanup/removal predicate unless the selected baseline
  already has, or the same slice adds, the minimal producer for known graceful
  cleanup/removal. If no producer is present, drop 6A0 rather than waiting on a
  state no code can produce.
- If that producer is missing, the only allowed same-slice producer is the
  minimal `330a29f` acknowledged self-unpublish before local teardown, in
  service mode, without user-handler dispatch. Do not defer it to a later slice
  after keeping 6A0.
- Start without drain attribution maps. Add timeout attribution only if
  diagnostics are needed after the predicate is correct.

### Slice 5: Shutdown/User-Barrier Membership

Reimplement narrowly from `ae7fed3` only if the baseline still has the bug:

- Collective shutdown participants leave user barriers but stay in internal
  lifecycle barriers.
- Stale process-slot state is reset on activation/reuse.
- Forged `begin_collective_shutdown` calls are rejected.

Do not fold this into group membership authority.

### Slice 6: Group Membership Authority

- Reimplement the invariant from `9473218` only if a test demonstrates duplicate
  membership authority is blocking cleanup correctness.
- This slice may contain same-scope membership-only helper changes.
- Do not touch group publication timing, group lifetime rollback, waiter
  batching, B2 publication, or broad barrier cleanup.
- If publication/rollback ownership is required, stop. The B2 design is owned
  by `docs/STALE_lifecycle_group_publication_rebuild_plan.md` and must be reopened as
  a separate reviewed design, not patched inside this recovery slice.

### Slice 7: Hard-Dead Process Retirement

- Preserve the current hard-dead test as quarry material, not as a settled gate.
  It currently depends on drain snapshots, blocker details, and test hooks that
  earlier slices may remove.
- During the baseline audit, decide whether this repro is rewritten against the
  reduced surface or classified as a false start.
- Treat the current hard-dead test oracle as provisional until the baseline
  audit confirms the intended classification. Reconcile any one-iteration local
  repro with the five-iteration active-test roster before using it as a gate.
- Before implementation, decide whether the current dirty hard-dead rewrite is
  quarry material or a false start. Preserve it first; then harvest only source-
  confirmed fixes and discard the rest.
- After any `2cbbb1c` reader/ring fragments, salvage from `f6dac12` the
  reader detach/stop rollback for failed startup and external reservation.
- First try to make retirement `not started` or `committed` using explicit
  identity `{process_iid, occurrence, reader}`. That identity should make stale
  readers impossible instead of adding a broad result matrix.
- Add a temporary or retryable retirement state only if the slice proves a hard
  commit boundary cannot represent a real interleaving. If that happens, record
  the proof and keep the retry path local to this slice.
- Do not key retirement only on raw `Process_message_reader*`.
- Do not combine this with mutex recovery, group publication, self-unpublish, or
  lifecycle finalization retries.

### Slice 8: Optional Self-Unpublish Teardown

Only if reproduced after earlier slices:

- If Slice 4 already kept the minimal `330a29f` producer, do not duplicate it
  here.
- From `edd32dc`, do not replay wholesale. Consider only terminal
  self-unpublish result handling, remote-coordinator/self-root terminal flags,
  and reserved-RPC sender validation if a focused repro requires them.
- Exclude the retry loop, expanded blocker matrix, and timeout diagnostics
  unless independently justified.

## What Not To Do

- Do not turn `Admission_boundary` into a lifecycle coordinator.
- Do not add result matrices for hypothetical future blockers.
- Do not add another active planning/control-plane document. Update this file
  with the baseline decision, then code.
- Do not add broad `#define private public` tests unless a smaller public
  regression test is impossible.
- Do not keep CI active-tests-only as the normal branch signal.
- Do not patch around the same class of review finding twice. Stop and shrink
  the slice.

## Review Rule

This recovery plan gets the current six-Codex-plus-Claude review because the
baseline is uncertain. Future slices do not inherit that ceremony.

Before coding a slice, one independent reviewer checks only scope, named gate,
and whether the slice stayed small. Use a larger review only when the baseline
changes, the slice becomes multi-domain, or the same blocker class repeats.

## Next Action

Do not code in the preserved dirty worktree. Create a clean implementation
worktree from `02f03bf`, run an architecture/scope review for Slice 1, then
implement the external invitation claim fix and its focused gate.
