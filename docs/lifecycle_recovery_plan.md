# Lifecycle Recovery Plan

Status: Slice 1 is complete. The red gate, production fix, tightened oracle,
independent review rerounds, Claude review, and focused platform CI are green.

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
- CI/test discipline finding: blocking CI now builds the branch-valid
  recent-failures roster in `tests/active_tests.txt`, plus focused tests for the
  active slice. Treat any full-suite run as background signal unless the user
  explicitly asks to wait for it.

Slice 1 architecture review record, 2026-07-09:

- Six xhigh Codex reviewers returned GREEN for a narrow Slice 1, with this
  invariant: `claim_external_process_invitation()` must serialize with the
  existing `s_teardown_admission_mutex`; if teardown admission is closed before
  the claim commit point, the claim returns `false` without consuming the
  invitation token or mutating registry, external-attached state, groups, or
  drain state. Wrong-token and expired-token rejection semantics stay as-is.
- Claude review artifact:
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\claude-slice1-arch-20260709\sintra_slice1_arch_review_20260709_023734\combined_reviews.md`.
  Claude agreed the production direction is narrow, but returned BLOCKER on the
  test gate: a shutdown-complete test is not enough. The gate must reproduce a
  claim attempt during teardown admission, before production code is changed.
- Base-path read on the clean implementation worktree:
  `create_external_process_invitation()` holds `s_teardown_admission_mutex`
  across reservation; `spawn_swarm_process()` holds it across direct-spawn
  argument/launch commit; RPC `Coordinator::join_swarm()` enters under the same
  mutex; recovery's actual `spawn_now()` commit is also mutex-guarded. The
  external claim path is the Slice 1 asymmetry: it checks only `m_shutdown` and
  erases the invitation before the reader, registry, group, and drain commits.
- Gate decision: first try a public black-box gate, not a private rendezvous.
  Use `shutdown_options.coordinator_shutdown_hook` to release the existing
  delayed external helper after `shutdown()` closes teardown admission and
  before `finalize_impl()` calls `Coordinator::begin_shutdown()`. The hook must
  wait for the helper to attempt `sintra::init()`, observe rejection, and verify
  the original invitation is still cancellable. This is the chosen deterministic
  public interleaving.
- If that test does not fail on unmodified `02f03bf`, stop. Do not patch
  production. Amend this plan either to allow exactly one narrow claim-commit
  test rendezvous or to drop/re-scope Slice 1.
- Production eligibility: only after the new gate is observed red on
  unmodified `02f03bf` may the production claim fix be implemented.
- Red-gate evidence, 2026-07-09: the public hook-based test was added to
  `tests/external_process_invitation_lifecycle_negative_test.cpp` with no
  production changes. Local Ninja configure was unavailable because `ninja` was
  not on `PATH`; a MinGW focused build using `-Wa,-mbig-obj` succeeded. Running
  `build\slice1-focused-mingw-bigobj\tests\sintra_external_process_invitation_lifecycle_negative_test.exe`
  on unmodified production returned exit code `1` and failed the two new
  assertions: `shutdown-hook helper init should be rejected while teardown
  admission is closed` and `shutdown-hook rejected claim should leave the
  original invitation cancelable`. The captured rerun log is
  `build\slice1-focused-mingw-bigobj\slice1_red_gate_rerun.txt` in this
  worktree.
- Green-gate evidence, 2026-07-09: the production fix now serializes
  `claim_external_process_invitation()` with `s_teardown_admission_mutex` and
  erases a valid invitation only after the full external attach commit succeeds.
  The focused MinGW bigobj target rebuilt successfully and
  `build\slice1-focused-mingw-bigobj\tests\sintra_external_process_invitation_lifecycle_negative_test.exe`
  exited `0`.
- Review amendment, 2026-07-09: the first production-review round found that
  the new hook oracle accepted generic `init_failed`. The gate now classifies
  the exact `Sintra external process invitation was rejected.` exception as
  `rejected`; other init failures remain acceptable only for the older
  post-shutdown delayed-helper case and fail the hook case.
- Tightened-oracle evidence, 2026-07-09: applying only the test tightening to
  pre-production commit `6cf1ce1` built successfully in
  `C:\plms\bsd_licensed\sintra-slice1-red-tight-20260709-0329` and exited `1`
  with the two hook failures. The log is
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\slice1_tightened_oracle_red_6cf1ce1.txt`.
  Rebuilding and rerunning the same target on the production branch exited `0`;
  the green log is
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\slice1_tightened_oracle_green_9ae88cb_plus_fix.txt`.
- Independent review and CI evidence, 2026-07-09: the final six-Codex reround
  against `7634b53` returned GREEN on correctness, architecture, test oracle,
  lock ordering, integration/CI readiness, and governance/defer-trap. The Claude
  review batch at
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\claude-slice1-production-20260709\sintra_slice1_production_review_20260709_20260709_031659\combined_reviews.md`
  returned GREEN with only low follow-ups. Push CI on
  `7634b5344e0ea0c934b82949f2df84aa009a33d9` succeeded on Linux
  (`28988282975`), macOS (`28988282969`), Windows (`28988282967`), and FreeBSD
  (`28988282901`).

## Implementation Slices

Each slice has one objective, one focused test or existing test target, and no
speculative public/test helper surface. If a slice needs broad new state, stop
and split it.

For every conditional slice, record a durable yes/no decision in this file
before skipping it: whether the selected baseline exhibits the bug, what
evidence was checked, and which focused gate covers the decision.

### Slice 0: CI And Test Discipline

- Blocking CI for this recovery branch is the shortened recent-failures suite in
  `tests/active_tests.txt`, plus focused tests for the active slice. Full-suite
  CI may be launched for background safety signal, but do not block orchestration
  on it unless the user explicitly asks.
- `bf3ba2e` is CI/test-selection only. Keep its CMake focus mechanism and
  workflow activation, but keep the roster branch-valid; do not copy entries for
  discarded refactor-line tests that do not exist in this recovery branch.
- Before claiming a slice green, the shortened blocking CI must compile and run
  on Linux, macOS, Windows, and FreeBSD unless a recorded user instruction or CI
  outage narrows that requirement.
- Blocking stress-test jobs should use a global `run_tests.py --time-budget`
  cap so repetition-heavy focus rosters cannot turn CI into an unbounded wait.
  Budget exhaustion is green only after each selected test has at least one
  passing run; `did_not_run` does not satisfy the budget cap.
- `active_tests.txt` entries missing from the build are fatal runner errors.
  The shortened gate is invalid if it silently runs only the subset of active
  tests whose binaries happened to be present.
- Triage `ee15fec` and the dirty edits to `tests/active_tests.txt`,
  `tests/runner/configuration.py`, and `tests/external_process_invitation_test.cpp`
  here or in the admission slice. Do not leave them as unowned cleanup.

### Slice 1: External Claim Admission

Owns only the original external invitation/claim hole on the chosen baseline.
This slice is not the bucket for every independent salvage fix.

- First add the red gate for a claim attempt while teardown admission is closed
  but before shutdown completion. Only after observing that gate fail on
  unmodified production, fix the narrow existing claim path.
- The production fix may touch only the external-claim commit boundary unless a
  reviewer accepts a recorded amendment.
- After the tightened oracle and production review reround are green, run the
  normal CI gate from Slice 0 before advancing.

### Slice 1A: Static Teardown Registry Lifetime

- Reproduce or audit the base bug from `a29dfbc`: static teardown registry
  lifetime and `deactivate_all` mutation while iterating.
- Keep it independent of external invitation admission, process retirement,
  drain wait, and mutex recovery.

Slice 1A scope review record, 2026-07-09:

- Six Codex architecture/scope lanes returned GREEN_TO_IMPLEMENT on clean
  `d196276`: the `a29dfbc` hazards remain present by audit. The cleanup guard
  installed by `init()` can own process-exit finalization; teardown then reaches
  handler/RPC registries and the Windows semaphore handle cache that are still
  destructible function-local statics. `Transceiver::deactivate_all()` also
  invokes `m_deactivators.back()` while the invoked deactivator can erase that
  list node.
- Focused gate: make a test-only change to
  `tests/handler_lifetime_test.cpp` by adapting the missing `a29dfbc` cases:
  stable copied deactivators through `deactivate_all_slots()`, and a
  no-explicit-shutdown process-exit cleanup-guard path with live slots. The
  cleanup-guard path is the required red proof for this slice. The
  `deactivate_all_slots()` case may stay as a smoke/hardening assertion, but it
  is not sufficient red evidence if it remains green. Prefer an ASan build for
  the cleanup-guard red gate; if no deterministic red can be produced, stop and
  amend or drop Slice 1A before touching headers.
- Production scope after red evidence only:
  `include/sintra/detail/transceiver.h`,
  `include/sintra/detail/transceiver_impl.h`, and Windows
  `include/sintra/detail/ipc/semaphore.h`, limited to process-lifetime
  teardown-touched statics and stable deactivator copies in
  `Transceiver::deactivate_all()`. The static conversion list must include
  `handler_slot_states()`, `Transceiver::get_rpc_handler_map()`,
  `Transceiver::get_instance_to_object_map<RPCTC>()`, and `ips_win_handles()`.
  Do not add `exception_conversions_impl.h` unless the red-gate work proves
  that exception conversion is reachable on the no-shutdown exit path.
- Out of scope: coordinator changes, external invitation admission, teardown
  admission mutexes, process retirement, drain/finalize redesign, process
  liveness/mutex recovery, child-shutdown oracle work, group
  membership/publication, CMake/workflow/active-test changes, new public hooks,
  and lifecycle blocker/result matrices.
- Claude architecture review, 2026-07-09:
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\claude-slice1a-arch-20260709\sintra_slice1a_arch_review_20260709_20260709_042432\combined_reviews.md`
  returned GREEN_TO_IMPLEMENT for the red-first step with the calibration above:
  do not force the formal `deactivate_all()` hazard to be red, and use the
  cleanup-guard path as the real red proof.
- Red-gate evidence, 2026-07-09: the test-only gate in
  `tests/handler_lifetime_test.cpp` adds a self-exec child that initializes
  Sintra, arms live slots, and exits without explicit shutdown so the static
  cleanup guard owns finalization. Normal MinGW bigobj focused build/run stayed
  green, as expected for this sanitizer-dependent lifetime bug; log:
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\slice1a_handler_lifetime_normal_after_child_argv_cleanup.txt`.
  MinGW and clang/GNU ASan configurations could not link an ASan runtime, and
  WSL lacked local CMake tooling. A fresh MSVC ASan NMake build succeeded and
  the focused test exited `1` because the no-explicit-shutdown child failed;
  logs:
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\slice1a_handler_lifetime_msvc_asan_build_after_child_argv_cleanup.txt`,
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\slice1a_handler_lifetime_msvc_asan_red_after_child_argv_cleanup.txt`,
  and
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\slice1a_handler_lifetime_msvc_asan_child_direct_after_child_argv_cleanup.txt`.
  Production headers remained untouched at the red gate.
- Red-gate review record, 2026-07-09: six Codex reviewers independently
  returned GREEN before the production header worker started. The lanes covered
  oracle correctness, sanitizer evidence, portability/integration, complexity,
  governance, and expected production fit. Governance repair note: this durable
  record was added after the production commit when production review found the
  missing plan artifact; the review itself occurred before production work.
- Green-gate evidence, 2026-07-09: the scoped production header fix rebuilt
  and ran `sintra_handler_lifetime_test` in the normal MinGW focused dir and
  the MSVC ASan NMake focused dir. MinGW build/run exited `0`/`0`; logs:
  `build\slice1-focused-mingw-bigobj\slice1a_production_fix_mingw_build.log`
  and
  `build\slice1-focused-mingw-bigobj\slice1a_production_fix_mingw_run.log`.
  MSVC ASan build/run exited `0`/`0`; logs:
  `build\slice1a-asan-msvc\slice1a_production_fix_msvc_asan_build.log` and
  `build\slice1a-asan-msvc\slice1a_production_fix_msvc_asan_run.log`.
- Independent review and CI evidence, 2026-07-09: the final six-Codex
  production reround against `ccce56f` returned GREEN on lifetime correctness,
  production boundary, red-to-green evidence, Windows/cross-platform readiness,
  complexity, and governance/defer-trap. Push CI on
  `ccce56fe7589f143f1d9845a11c3cbecf8360a0b` succeeded on Linux
  (`28991838368`), macOS (`28991838406`), Windows (`28991838378`), and FreeBSD
  (`28991838352`).

### Slice 1B: Process Liveness And Mutex Recovery

This slice is split. `5cd9149` grouped several concerns that do not share one
focused gate, and the first Slice 1B architecture review found that copying the
quarry mutex side-channel design would add a new race.

Slice 1B scope review record, 2026-07-09:

- Six Codex architecture lanes did not converge on the original combined
  Slice 1B. They agreed the branch should not copy `5cd9149` wholesale. The
  useful current amendment is to split process liveness, mutex owner identity,
  and lifeline release ordering into separate decisions.
- Claude review artifact:
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\claude-slice1b-scope-20260709\sintra_slice1b_scope_review_20260709_20260709_061945\combined_reviews.md`.
  Claude agreed the original Slice 1B is too broad and must be split before
  implementation. Its mutex quarry assessment lacked raw `git show` access, so
  future mutex decisions must cite local git evidence directly; the split and
  gate discipline remain accepted.
- Local git evidence confirms `git show 5cd9149 -- include/sintra/detail/ipc/mutex.h`
  does contain a start-stamp side-channel design. That design is not accepted
  for implementation here: a losing contender can invalidate owner identity
  after another contender wins and publishes it. Any future mutex fix must
  prevent losing contenders from mutating winner identity and must prove that
  invariant before production code lands.
- ABI decision for future mutex work: if `interprocess_mutex` layout or
  persisted recovery semantics change, bump branch-local
  `k_ring_abi_version` from `5` to `6` and
  `k_ring_lifecycle_anchor_abi_version` from `2` to `3`, because
  `interprocess_mutex` is embedded in both ring control and lifecycle anchor
  shared-memory objects. A process-utils-only liveness fix requires no ABI
  bump.

#### Slice 1B.1: macOS process liveness

- Red gate only: add a test-only POSIX child process check that forks, lets the
  child exit without reaping, asserts `!sintra::is_process_alive(child_pid)`,
  and then always reaps with `waitpid`.
- Required red evidence: macOS CI must fail the test-only patch for the
  intended reason, `exited unreaped child should not be reported alive`.
  Windows skips this path, and local Linux/FreeBSD results do not authorize the
  macOS fallback production change.
- Allowed production after red: `include/sintra/detail/ipc/process_utils.h`
  macOS liveness fallback only. No mutex, ring, or lifeline production changes
  are allowed in Slice 1B.1. Workflow or `active_tests.txt` changes are allowed
  only if CI proves the focused utility target is not running, and then only to
  run that target; they must not add or enable mutex, ring, or lifeline code.
- Stop condition: if macOS CI passes the test-only patch, or fails for fork,
  waitpid, timeout/flakiness, compile leakage, or any reason other than the
  liveness assertion, do not patch production. Record Slice 1B.1 as dropped or
  amend the gate.

Slice 1B.1 closure record, 2026-07-09:

- Red gate commit `7c01627` added the POSIX unreaped-child liveness oracle in
  `tests/utility_test.cpp`. macOS CI run `28994505589` failed for the intended
  assertion, `utility_test: exited unreaped child should not be reported alive`.
  Linux run `28994505545`, FreeBSD run `28994505371`, and Windows run
  `28994505519` completed successfully for the same red-gate commit.
- Production commit `9db0b08` changed only
  `include/sintra/detail/ipc/process_utils.h`, adding a macOS-only liveness
  fallback that treats zombie or missing `KERN_PROC_PID` evidence as not alive
  and leaves unknown evidence conservative.
- Six xhigh production reviewers accepted the code and scope; one closure-only
  blocker required durable evidence in this plan. CI discipline was amended in
  `4390b4a`, `01c7246`, and `a064cc6` to restore a branch-valid focused CI
  gate, cap repetition-heavy stress runs, reject `did_not_run` budget greens,
  and fail if any active-test binary is missing from the requested build
  configuration set. Those CI changes had separate reviewer gates, including
  blocker/rerun checks for budget and missing-binary semantics.
- Focused blocking CI for `a064cc6` completed successfully on Linux
  `29002418892`, FreeBSD `29002418747`, Windows `29002418884`, and macOS
  `29002418901`.
- Slice 1B.1 is closed. Do not add mutex, ring, lifeline, drain, or lifecycle
  gate work to this slice after closure.

#### Slice 1B.2: interprocess_mutex owner-generation recovery

- Architecture decision, 2026-07-09: attempt a red gate first; do not implement
  production mutex recovery until that gate is observed red for the intended
  stale-generation reason and independently accepted as a valid oracle.
- Six Codex xhigh architecture lanes and the Claude review agreed that
  `5cd9149` must not be cherry-picked. The current mutex owner token is only
  `{pid, tid}` and recovery uses `is_process_alive(owner_pid)`, so PID/TID reuse
  can leave a stale owner token looking live. Ring attachment start stamps do
  not cover this path because the lifecycle mutex must be locked before
  attachment scavenging can inspect those stamps.
- Claude architecture review artifact:
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\claude-slice1b2-arch-20260709\sintra_slice1b2_arch_review_20260709_20260709_105813\outputs\slice1b2_mutex_arch.md`.
  Claude returned BLOCKED_NEEDS_GATE and warned that a synthetic
  stale-generation test is valid only if it is treated as the source-confirmed
  PID/TID-reuse hazard, not as a test laundering a new feature. If that oracle
  cannot be made red and accepted without storage-mechanic hooks, durably drop
  Slice 1B.2.
- Red-gate scope: `tests/interprocess_mutex_test.cpp`,
  `tests/active_tests.txt`, and `include/sintra/detail/ipc/mutex.h` only for
  semantic `SINTRA_ENABLE_TEST_HOOKS` support needed to seed/read mutex owner
  identity. Do not touch `rings.h`, `process_utils.h`, runtime, process,
  lifeline, drain, coordinator, or admission files in the red gate.
  Add `interprocess_mutex_test 1` to `tests/active_tests.txt` for the red-gate
  attempt because blocking CI builds only the active roster.
- Red-gate oracle: using a nonzero stale start stamp different from
  `current_process_start_stamp()`, seed `{current_pid, different_nonzero_tid,
  stale_start_stamp}` and require `try_lock_for()` to recover it; seed
  `{current_pid, current_tid, stale_start_stamp}` and require recovery instead
  of recursive/self-owner refusal. Also seed both
  `{current_pid, different_nonzero_tid, current_start_stamp}` and
  `{current_pid, current_tid, current_start_stamp}`, and require that
  `try_lock_for()` does not recover either live-current-generation owner. The
  two current-start-stamp cases are safety guards and must pass on both red and
  green revisions. Expected red on current production: the stale-generation
  cases time out or are refused because the mutex has no owner-generation state
  and only checks PID liveness.
- Test hooks must be semantic. Allowed examples are installing an owner identity
  fixture `{pid, tid, start_stamp}` and reading the owner token for assertion.
  Forbidden hooks include raw atomic/offset access, separate side-channel
  token/stamp APIs,
  `test_set_raw_owner_with_side_channel()`, `test_invalidate_owner_identity()`,
  and any hook that exposes implementation storage mechanics.
- Production candidate after accepted red evidence only: a one-stamp mutex
  design. Add exactly one owner start-stamp field. Only a thread/process that
  has won the owner CAS may publish a nonzero stamp; failed contenders must not
  write or clear it. Unlock must CAS `m_owner` from `self` to `0` and must not
  touch the owner stamp; the gated unowned acquisition path clears stale
  unowned stamps before publishing the next owner.
  Acquisition must acquire `m_recovering` before publishing a new owner and
  must not publish while another thread/process holds that gate. While holding
  the gate, acquisition must clear any old unowned stamp immediately before
  CASing `m_owner` from `0` to `self`.
  Recovery must hold `m_recovering`, re-read `m_owner == observed_owner`, CAS
  that same owner token to `0`, and clear only the exact stamp value it
  observed, only after that CAS succeeds and while it still owns the same
  `m_recovering` gate; if the owner token, stamp, or gate changed, restart or
  leave the stamp untouched. Recovery/publication gates may be stolen only from
  dead gate-owner processes; live stalled gate holders are not preempted,
  because same-token owner reuse makes both destructive recovery and owner-stamp
  publication unsafe to steal from a live holder. Same-token recursion may be
  classified
  only after a fresh snapshot confirms that recovery is not active and
  `m_owner` still equals `self`. Recover only when the owner PID is dead, or
  when the stored stamp is nonzero,
  `query_process_start_stamp(owner_pid)` returns a value, and that value differs
  from the stored stamp. A zero stored stamp or unavailable queried stamp is
  unknown evidence and must not recover a live PID. The design deliberately does
  not recover the crash window after owner CAS and before stamp publication;
  closing that window requires another accepted architecture review. This
  candidate intentionally avoids `owner_identity`, `m_owner_stamp_token`,
  `try_claim_unowned`, and loser-contender invalidation from `5cd9149`.
- If production changes `interprocess_mutex` layout or persisted recovery
  semantics, bump branch-local `k_ring_abi_version` from `5` to `6` and
  `k_ring_lifecycle_anchor_abi_version` from `2` to `3`; do not copy the
  sibling-line ABI numbers from `5cd9149`. Green gate must include
  `sintra_interprocess_mutex_test`, `sintra_ring_abi_fingerprint_test`,
  `sintra_utility_test`, and focused platform CI with `interprocess_mutex_test`
  and `ring_abi_fingerprint_test` present in `tests/active_tests.txt`.
- Stop conditions: if the red gate passes current production, fails for a
  reason other than stale-generation non-recovery, requires actual PID/TID reuse
  or flaky timing, or needs storage-mechanic hooks, stop and record Slice 1B.2
  as dropped or redesign the gate through another architecture review.
- Red-gate evidence, 2026-07-09: commit `9894b12` added the semantic owner
  fixture hook and owner-generation oracle with no production recovery logic.
  The focused MinGW debug target `sintra_interprocess_mutex_test` exited `1`
  with only the intended stale-generation failures:
  `stale-generation owner with current pid and different tid should recover`,
  `stale-generation owner with current pid and current tid should recover`, and
  `owner-generation recovery red gate failed`. The active-only MinGW build also
  built the newly active mutex test and produced the same red output.
- Green-gate evidence, 2026-07-09: the production fix uses the reviewed
  one-stamp mutex design, bumps branch-local ring ABI `5 -> 6` and lifecycle
  anchor ABI `2 -> 3`, and adds `ring_abi_fingerprint_test` to the active
  roster. Local active-only MinGW build/run succeeded for
  `sintra_interprocess_mutex_test`, `sintra_ring_abi_fingerprint_test`, and
  `sintra_utility_test`.
- Final review evidence, 2026-07-09: six xhigh lanes returned green after the
  production diff removed live stalled-gate preemption, made recovery clear
  only the exact observed stamp, and changed `unlock()` to leave stamp cleanup
  to the gated unowned-acquisition path.
- Focused platform CI evidence, 2026-07-09: commit `3960b3e` passed the active
  `Build & Test` workflows on Linux (`29011919650`), FreeBSD (`29011919518`),
  Windows (`29011919631`), and macOS (`29011919564`). This closes Slice 1B.2;
  it is not evidence that the comprehensive suite is green.

#### Slice 1B.3: lifeline release ordering

- Durable drop decision, 2026-07-09: do not implement Slice 1B.3 on the
  current baseline.
- Six Codex architecture reviewers did not produce a safe production-green
  consensus: three required a focused gate, two allowed a tiny source-audit
  implementation, and one recommended drop. The accepted governance decision is
  to prefer the smallest source-confirmed path and avoid a source-audit
  exception where reviewers did not converge.
- Claude architecture review artifact:
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\claude-slice1b3-arch-20260709\sintra_slice1b3_arch_review_20260709_20260709_103639\outputs\slice1b3_lifeline_arch.md`.
  Claude returned DROP and supplied the deciding source-audit objection: the
  `5cd9149` lifeline move is not atomic on this baseline. The quarry moved
  `release_all_lifelines()` after a `spawned_child_process_created` directory
  retention gate, while this branch has no such gate and removes
  `m_directory` unconditionally in `Managed_process::~Managed_process()`.
  Moving the lifeline release alone could leave children alive while the parent
  unlinks their ring files.
- Current-source evidence: `Managed_process::~Managed_process()` still releases
  lifelines before local reader/writer teardown, but the named quarry rationale
  depends on exit-time ring lifecycle lock recovery while a child is becoming
  observable as dead on macOS. The branch already has the base robust mutex
  owner-death recovery path, and Slice 1B.1 fixed the macOS process-liveness
  predicate consumed by that recovery. No residual hang or corruption repro has
  been produced for this baseline.
- The `runtime.h` half of `5cd9149` is not accepted as 1B.3 work. The current
  `cleanup_failed_init_noexcept()` no longer calls `release_all_lifelines()`
  directly, and the destructor call is idempotent in any case. If a future slice
  legitimately edits failed-init cleanup, it must re-audit that path locally.
- This is a drop, not a defer. Reopen only if a real red repro of an
  exit-time ring-lifecycle-mutex stall or corruption is produced on the current
  ordering. If Slice 1B.2 changes mutex owner-death/recovery semantics, it may
  only record a new 1B.3 risk; lifeline ordering and any required directory
  retention rule must be handled by a separate focused slice/review. Do not
  import the standalone `5cd9149` lifeline hunk.
- No `managed_process_impl.h` or `runtime.h` lifeline ordering changes may ride
  along with process liveness, mutex recovery, child-shutdown oracle work, or
  later drain/finalize work.

### Slice 1C: Child Shutdown Test Oracle

- Reapply the `64dc45f` child-shutdown failure propagation test only if the
  baseline still swallows the failure.
- This is test-oracle work; do not combine it with production lifecycle
  scaffolding.
- Durable drop decision, 2026-07-09: do not keep the adapted
  `choreography_extreme_test` status-file oracle on this baseline.
- Evidence: the exact `64dc45f` patch targets the sibling repair line and
  depends on finalize-blocker types absent from this branch. The adapted
  bool/exception-only oracle never produced the intended red signal: no child
  `sintra::shutdown()` false/throw status was observed and swallowed by the
  root. The unmodified test passed 10/10 under a 30-second per-run harness,
  while the adapted oracle timed out by iteration 4 under the same harness.
  That timeout is a different liveness signal, not accepted Slice 1C evidence.
- Review evidence: the first six-Codex review round split 2 keep, 2 drop, 2
  narrow. Claude returned `RED_DROP_ORACLE` in
  `C:\plms\bsd_licensed\sintra-lifecycle-artifacts\claude-slice1c-decision-20260709\sintra_slice1c_decision_review_20260709_20260709_132913\outputs\slice1c_decision.md`.
  A second six-Codex convergence round returned `GREEN_DROP_RECORD`.
- Reopen only with deterministic intended-reason red evidence on this baseline:
  a child `sintra::shutdown()` genuinely returns `false` or throws, and the
  unmodified root demonstrably fails to surface that child shutdown failure.
  Do not treat the observed repetition timeout as this slice's oracle; if that
  liveness symptom is pursued, it must be a separately scoped reproduced-first
  slice.

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
- Own any independent `2cbbb1c` fragments here: ring writer-owner-lost
  detection and reader `shared_from_this` lifetime. Do not carry the retirement
  subsystem/result matrix unless this slice's focused gate proves it necessary.
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
- Do not treat the focused active-tests CI as proof that the full suite is
  clean, merge-ready, or release-ready. It is the blocking signal for this
  recovery branch only.
- Do not patch around the same class of review finding twice. Stop and shrink
  the slice.

## Review Rule

This recovery plan gets the current six-Codex-plus-Claude review because the
baseline is uncertain. Future slices do not inherit that ceremony.

Before coding a slice, one independent reviewer checks only scope, named gate,
and whether the slice stayed small. Use a larger review only when the baseline
changes, the slice becomes multi-domain, or the same blocker class repeats.

## Next Action

Do not code in the preserved dirty worktree. Slice 1A and Slice 1B.1 are
complete. Slice 1B.3 is durably dropped. Slice 1B.2 red-gate and local green
gate are complete, and the production diff passed six-reviewer validation.
Focused platform CI is green. Slice 1C is durably dropped. Next action is a
Slice 2 scope/architecture check: verify whether any minimal admission repair
remains on this branch before coding.
