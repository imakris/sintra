# Lifecycle Recovery Plan

Status: Slices 1, 1A, 1B.1, 1B.2, 2, and 5 are closed; Slices 1B.3,
1C, 3, and 4 are durably dropped. The test/CI recovery remediation is closed at
`266fa2b` with local green evidence and full-selection CI green on Linux,
FreeBSD, macOS, and Windows. Slice 6 remains conditional and cannot open before
the protected A0 evidence/successor sequence closes.

Sibling-track status, 2026-07-11, is derived from validated repository state:
Batch 0/1 is **closed** and Batch 2 (or a later custody descendant) is authorized
if and only if current HEAD's first-parent ancestry contains exactly one valid
closure commit and the fresh live protection predicate in **Finite Batch 0/1
Closure And Batch 2 Authorization** also passes. Before that commit, or on any
missing/multiple/mismatched ancestry or live-state fact, Batch 0/1 is **open**
and Batch 2/continuation is blocked. This descendant-safe predicate, not an
earlier prose snapshot or chat verdict, is authoritative. The custody track does
not change the legacy closed/drop decisions or reorder the protected A0 evidence
-> managed transport successor -> fence successor -> conditional later-slices
sequence.

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
- `436e7f2`: repair sibling 4A closure record. It is doc-only; the actual 4A
  RPC terminal-state code is in `a5f7f72` and is entangled with runtime blocker
  machinery. Do not select it merely because the range has useful-looking RPC
  terminal-state work.
- `1cf7239`: repair sibling post-6A0 closure record. Candidate only if the audit
  proves the 4B lifecycle expansion is removed or independently safe. The actual
  6A0 code carrier is `55918b4`, not `1cf7239`.

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
- `a5f7f72` and `55918b4` remain quarry references for 4A RPC terminal cleanup
  and the narrow 6A0 drain idea. They are not baselines because the deletion
  burden is larger than cross-grafting the useful pieces. `436e7f2` and
  `1cf7239` themselves are closure records, not the code carriers.
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

## Managed-Child Custody Sibling Program

### Material Amendment Record And Scope

This section is the one durable plan amendment for Sintra-managed child custody.
It is a sibling track inside this canonical lifecycle plan, not a second plan and
not a restoration of historical salvage primitives. Its purpose is to close the
confirmed gap between OS child creation and durable Sintra ownership, then move
the framework to the same semantic contract. The target may expose launch,
observation, release, retry, and wait operations over one opaque custody
identity; this amendment intentionally does not choose function, type, status,
or result-matrix spellings before the RED evidence and Batch 3 design exist.

The sequencing below is a material amendment. Earlier convergence material
placed framework async-start evidence beside the initial Sintra RED work. Source
diagnosis now proves that framework `3a69bfc` cannot compile against Sintra
`c60c104` because it consumes five absent salvage surfaces. An R4 executable
built before dependency decontamination would therefore test a fabricated shim,
an unrelated revision, or no runnable pair. Because Batch 3 source-breakingly
deletes the scalar launch/cleanup composition, dependency pinning alone also
cannot create an intermediate buildable pair. The amended sequence keeps R4 at
source diagnosis only in Batch 2; Batch 4A pins exact Batch 3 Sintra, migrates
every synchronous and asynchronous framework custody call site, and deletes all
raw/name/salvage cleanup authority in the same atomic source-breaking cutover,
while intentionally preserving only the existing async admission/quiescence
defect. Batch 4B adds the real-child causal R4 RED on that exact migrated pair
with join/detach/reset behavior unchanged. Batch 4C changes async admission/join
and deletes newly orphaned R4-only seams/adapters/helpers. No temporary adapter or
unbuildable transition is permitted. R3 is also narrowed to the Sintra-only
failed-readiness case described below so it does not duplicate R5. The fourteen
fixed anchors remain unchanged.

Provenance, not a closure verdict:

- The accepted online validation at
  `C:/plms/stress_artifacts/sintra_online_lifecycle_review_validation_20260711.md`
  returned `ARCHITECTURE VERDICT: CONVERGED` after its critical source claims
  were checked locally.
- The earlier convergence report at
  `C:/plms/stress_artifacts/sintra_owned_child_custody_convergence_20260711.md`
  established the custody direction but remained AMBER pending this durable
  amendment and causal baseline evidence.
- The rejected framework patch, blocker statement, and blocked review remain
  evidence only. They do not authorize copying
  `Force_process_release_result`, `Degraded_shutdown_result`,
  `force_release_process()`, `force_release_known_processes_for_degraded_shutdown()`,
  or `spinlocked_umap::try_scoped()`.
- The initial Batch 0/1 diagnosis covered architecture/plan,
  dependency-pair, future evidence/migration, and governance/state lanes. It
  produced the R3 and ordered 4A/4B/4C amendments recorded here. The exact
  execution, remediation, verification, review, commit, and still-open boundary
  state are recorded below; earlier candidate-level GREEN does not convert the
  non-unanimous first boundary round into closure.

### Batch 0/1 Execution, Verification, And Review Record

All named workers, verifiers, and reviewers in this record ran as
`gpt-5.6-sol`, xhigh reasoning, priority service tier. With the orchestrator
occupying one of four active slots, each maximum review round used all three
available fresh independent worker slots. `b01_plan_author` authored and
remediated the plan; no author served as verifier or reviewer. Verifiers and
reviewers received frozen candidates, and completed agents were closed before
fresh rounds.

Chronology and frozen identities:

1. The initial maximum pre-author pool returned:
   `b01_architecture_plan` **RED as expected before an author candidate**;
   `b01_dependency` **GREEN**; and `b01_evidence_governance` **RED** on the
   original R3/R4 ordering. This was diagnosis, not candidate closure.
2. Focused diagnosis followed. `r34_harness_feasibility` returned
   **INFEASIBLE** for a lawful framework R4 harness before dependency
   decontamination. `r34_batch_order` and `r34_architecture_resolution` supplied
   the adopted resolution: keep R4 source-only in Batch 2, make R3 a distinct
   Sintra gate, and order the truthful framework 4A/4B/4C cutover recorded here.
3. `b01_plan_author` froze the initial one-file author patch at SHA-256
   `5812E249F34FD73572959A85A3B0EA23FF7DE4B21B085C4A4353843A35A73BA8`;
   the historical working-file SHA-256 (non-gating for durable closure) was
   `775375FF22CA011BEAA3982A9002D63557C325E2544166F63452B89ECA6DD558`.
   Independent `b01_plan_verifier` returned **RED** with four findings:
   impossible pre-create native-identity timing; reconciliation incorrectly
   requiring A0 disposition before either track could land; R7/R8 lacking exact
   source-supported causal baseline outcomes; and the legacy one-reviewer rule
   not explicitly subordinated to custody's maximum-fresh/unanimous protocol.
4. The author remediation froze patch SHA-256
   `D135CBDFB32BF2E951476795D78EB5D7BE9507935A75992C19A722C5474E222A`
   and historical working-file SHA-256 (non-gating for durable closure)
   `C94E934EFEE1262926316E19C8DFC6414EB6053159C56CE8FE320F6A6EFD7B33`.
   Independent `b01_plan_verifier_r2` returned **GREEN**.
5. The first maximum final-candidate review returned
   `b01_final_arch_review` **GREEN**,
   `b01_final_governance_review` **GREEN**, and
   `b01_final_evidence_review` **RED** with two material findings: the 4A
   pin-only/custody-consumption split created an unbuildable defer trap after
   Batch 3's source break, and R7 falsely implied lifeline release for a
   lifeline-disabled child.
6. Remediation made 4A the complete source-breaking framework custody call-site
   cutover/deletion atom, preserved only the async admission/quiescence defect
   for 4B RED, restricted 4C to async repair/orphan deletion, and corrected R7
   to authoritative lifeline absence. The final candidate was 53815 bytes,
   patch SHA-256
   `B77623DCDD8F93DE5300C05D0698DE102ADC1AB374B4FBD98925E1A3C45065CD`,
   with historical working-file SHA-256 (non-gating for durable closure)
   `44940F6FE6E9439EC3B7988841A56E0E1AAC67ABD12F218794058ACDCD573CFF`.
   Independent `b01_plan_verifier_r3` returned **GREEN**.
7. The second maximum final-candidate review was unanimously unqualified
   **GREEN**: `b01_final_arch_review_r2`, `b01_final_evidence_review_r2`, and
   `b01_final_governance_review_r2`.
8. The exact reviewed one-file candidate was committed locally and unpushed as
   `cdd2d4a1cdcb3dca4dbcf575ad3d94ca73a443b5`, parent
   `c60c1045dfc2382609eae36afbd637fe274b2871`, with 851 insertions and 8
   deletions. The worktree was clean after commit. The commit used `--no-verify`
   because this was documentation-only, its static verification was delegated,
   and no compiler/build/test gate applied; this was not a waiver of any
   applicable verification.
9. The first maximum cumulative boundary round was **not unanimous**.
   `b01_boundary_arch` returned **RED** and `b01_boundary_governance` returned
   **RED**, both solely because the repository lacked this durable execution and
   closure record. `b01_boundary_evidence` returned **GREEN**. No other boundary
   defect was reported, but the two RED verdicts keep Batch 2 blocked.
10. The first closure-record candidate was 14007 bytes with patch SHA-256
    `92ED0AE58D7C75E5FF7B58279C8CB791CCD9E7C2EB77F629DE357107877D3823`;
    its historical working-file SHA-256 (non-gating) was
    `335DF1141069ECC72474B04320F3E1C076ADFC62C499560277730EB9BCA4B890`.
    The second cumulative boundary round was also **not unanimous**:
    `b01_boundary_arch_r2` returned **RED** with two findings—status/Next Action
    would remain stale-open after a valid closure commit, and the required plan
    identity used ambiguous working-file bytes instead of the raw Git blob.
    `b01_boundary_evidence_r2` and `b01_boundary_governance_r2` both returned
    **GREEN**. This remediation addresses those two findings; it does not convert
    that historical round to unanimous GREEN.
11. The transition-safe/raw-blob candidate was 17520 bytes with patch SHA-256
    `A5AC6FB617B3E2559EA98F5074815FADB9EA70955CFADD560A41181E06D89D3E`;
    its raw staged Git blob was 121734 bytes with SHA-256
    `2EFA3D9786D61721D5F1DC252D42DC9BB832EF472A0AB9AA5FCB6935ED9DE96C`.
    The third cumulative boundary round was **not unanimous**:
    `b01_boundary_arch_r3` and `b01_boundary_governance_r3` returned **GREEN**;
    `b01_boundary_evidence_r3` returned **RED** because a current-HEAD-only
    predicate would reopen Batch 0/1 immediately after the first Batch 2 commit.
    This remediation moves identity to one unique validated closure commit in
    first-parent ancestry plus fresh live protection checks. It does not convert
    the historical r3 round to unanimous GREEN.

Delegated verifier gate summary:

- Scope/state used `git status --short --branch`,
  `git diff --cached --name-status`, `git diff --cached --stat`, and
  `git diff --cached --numstat`; every frozen candidate contained only
  `docs/lifecycle_recovery_plan.md` and no unstaged file.
- Static patch validation used `git diff --cached --check` (exit `0`). Binary
  patch identity used
  `git diff --cached --binary --output=<temporary-file>`, `Get-Item` for exact
  bytes, and `Get-FileHash -Algorithm SHA256`. Historical working-file hashes
  above used `Get-FileHash -Algorithm SHA256 docs/lifecycle_recovery_plan.md`;
  they are diagnostic only. Durable closure instead hashes the raw staged and
  committed Git blobs byte-for-byte as specified below.
- Source searches checked the named Sintra/framework control flow, forbidden
  salvage surfaces, R3/R4/R7/R8 seams, dependency selection, and worktree/path
  scope. Positive searches returned their expected matches; absence searches
  returned `rg` exit `1` as the expected no-match result, not as a failure.
- A0 status plus `git diff --cached --binary` byte/hash pipelines ran before and
  after the plan batch and stayed at the exact seven paths, 155101 bytes, and
  SHA-256 recorded below. Canonical/worktree status and porcelain collision
  checks were recorded. The final process audit found no custody build, test,
  server, child, or verifier process left running.
- No compiler, configure, build, or test command was applicable to this
  documentation-only Batch 1 change. This no-build determination was itself
  independently verified; it is not missing evidence.

Current reconstructable state at the opening of this closure-record candidate:

- Dedicated custody worktree/branch is clean at local, unpushed
  `cdd2d4a1cdcb3dca4dbcf575ad3d94ca73a443b5`; its parent and immutable
  production base are `c60c1045dfc2382609eae36afbd637fe274b2871`.
  Roll back this closure-record atom to `cdd2d4a...`; roll back the whole custody
  plan branch to `c60c104...`.
- Canonical Sintra remains clean on `master` at
  `c60c1045dfc2382609eae36afbd637fe274b2871`, matching `origin/master`.
- Canonical framework remains tracked-clean on `master` at
  `3a69bfc8ccb8c0e42584542c96474113beeefbbb`. No framework custody worktree
  exists. Protected untracked `mexce_protected_pow_probe.obj` is 3102372 bytes,
  SHA-256
  `767F8C138768E87FEF1FDBF828B853D5D60D6307D721FA805D3256763B52331E`.
- Protected A0 remains branch `lifecycle-recovery-slice1`, HEAD
  `099d78a7e1e4e53f875c64d8f9f295f686d0a8fb`, with the exact seven staged
  paths listed below. Its binary patch remains 155101 bytes, SHA-256
  `CEC127D3C964E27268702EFC5654B8FB4AC9804BCA6938B4F5A3F86DCB54C03C`.
- The accepted online-review archive
  `C:/plms/stress_artifacts/sintra_framework_lifecycle_online_review_20260711.zip`
  remains evidence with SHA-256
  `163F8C91FFEC7A1457240F63BF62233A43E95C6BE0069B0787A09A093FD630F9`.

#### Finite Batch 0/1 Closure And Batch 2 Authorization

The current closure-record edit is not self-authorizing. Freeze one exact staged
identity pair: (a) the binary index diff against HEAD and (b) the raw Git blob
already stored in the index for `docs/lifecycle_recovery_plan.md`. Require
independent `b01_closure_verifier_r3` and a fresh maximum cumulative boundary
round consisting of `b01_boundary_arch_r4`, `b01_boundary_evidence_r4`, and
`b01_boundary_governance_r4`. All four must return unqualified **GREEN** on that
identical staged patch/blob pair. Any edit, identity change, or non-GREEN
restarts verification and the entire fresh boundary round.

The gating plan identity is SHA-256 of the raw Git blob, not the filesystem
working copy. Extract it byte-for-byte to an external temporary file; do not use
`Get-Content`, `Set-Content`, `Out-File`, or a PowerShell text pipeline. The
reproducible Windows/PowerShell sequence for the staged candidate is:

```powershell
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("custody-plan-" + [guid]::NewGuid() + ".blob")
$cmd = 'git cat-file blob :docs/lifecycle_recovery_plan.md > "' + $tmp + '"'
cmd.exe /d /s /c $cmd
if ($LASTEXITCODE -ne 0) { throw "staged blob extraction failed" }
(Get-Item -LiteralPath $tmp).Length
(Get-FileHash -Algorithm SHA256 -LiteralPath $tmp).Hash
```

The staged closure patch is generated with
`git diff --cached --binary --output=<external-temporary-file>` and hashed with
`Get-FileHash -Algorithm SHA256`. After commit, identity belongs to the selected
closure commit, not necessarily HEAD. Extract object
`<closure>:docs/lifecycle_recovery_plan.md` through the same byte-preserving
`git cat-file blob` redirection and require its SHA-256 to equal trailer
`Custody-Closure-Plan-Blob-SHA256`. Require exactly one first parent, verify it
equals `Custody-Closure-Parent`, regenerate with
`git diff --binary <closure>^ <closure> --output=<external-temporary-file>`, and
require that SHA-256 to equal `Custody-Closure-Patch-SHA256`. Raw blob bytes and
the parent-to-closure binary diff therefore remain reproducible after the index
and working file disappear and while HEAD advances through later descendants. A
working-file hash may be logged as a non-gating diagnostic only.

After the four required GREEN verdicts, commit those exact unchanged reviewed
bytes with message
`docs(lifecycle): record managed-child custody Batch 0/1 closure` and these Git
trailers, populated with the verifier-frozen staged values:

```text
Custody-Closure-Patch-SHA256: <exact reviewed staged/parent-to-closure binary patch SHA-256>
Custody-Closure-Plan-Blob-SHA256: <exact reviewed raw Git blob SHA-256>
Custody-Closure-Verifier: b01_closure_verifier_r3 GREEN
Custody-Boundary-Reviewer: b01_boundary_arch_r4 GREEN
Custody-Boundary-Reviewer: b01_boundary_evidence_r4 GREEN
Custody-Boundary-Reviewer: b01_boundary_governance_r4 GREEN
Custody-A0-Patch-SHA256: CEC127D3C964E27268702EFC5654B8FB4AC9804BCA6938B4F5A3F86DCB54C03C
Custody-Closure-Parent: cdd2d4a1cdcb3dca4dbcf575ad3d94ca73a443b5
Custody-Rollback: cdd2d4a1cdcb3dca4dbcf575ad3d94ca73a443b5
Custody-No-Build: doc-only; delegated static verification; no compiler/build/test applicable
```

Closure lookup is confined to current HEAD's first-parent ancestry. Enumerate it
with `git log --first-parent --format="%H%x09%s" HEAD`. For every commit whose
subject is exactly
`docs(lifecycle): record managed-child custody Batch 0/1 closure`, read the full
message with `git show -s --format=%B <commit>` and parse trailers with
`git interpret-trailers --parse`. Select only commits containing every required
trailer key above, then require exactly one selected commit. Zero or multiple
selected commits is a hard block; a later duplicate closure commit cannot
supersede the first by convention.

Call that unique commit `<closure>` and validate it reproducibly:

1. `git merge-base --is-ancestor <closure> HEAD` must exit `0`, and `<closure>`
   must occur in `git rev-list --first-parent HEAD`; second-parent-only ancestry
   is invalid.
2. `git rev-list --parents -n 1 <closure>` must contain exactly `<closure>` and
   one parent. That exact first parent must be
   `cdd2d4a1cdcb3dca4dbcf575ad3d94ca73a443b5` and must equal both the
   `Custody-Closure-Parent` and `Custody-Rollback` trailers.
3. Byte-preservingly extract raw object
   `<closure>:docs/lifecycle_recovery_plan.md` as specified above and require its
   SHA-256 to equal `Custody-Closure-Plan-Blob-SHA256`.
4. Run
   `git diff --binary <closure>^ <closure> --output=<external-temporary-file>`
   and require its SHA-256 to equal `Custody-Closure-Patch-SHA256`.
5. Require the exact verifier trailer `b01_closure_verifier_r3 GREEN`, the exact
   three-reviewer multiset `b01_boundary_arch_r4 GREEN`,
   `b01_boundary_evidence_r4 GREEN`, and
   `b01_boundary_governance_r4 GREEN`, with no missing, duplicate, substituted,
   or non-GREEN identity. Require the exact A0 hash and no-build trailers above.

Commit identity is necessary but not sufficient for descendants. At every
Batch 2 or later atomic/batch boundary, separately refresh the live protection
predicate: canonical Sintra remains at the recorded immutable baseline and
clean; A0 still has its exact seven staged paths, 155101-byte binary patch, and
recorded SHA-256 until separately reviewed disposition; canonical framework
remains at its recorded tracked-clean baseline; the protected OBJ retains its
recorded path, 3102372-byte size, and SHA-256; `git worktree list --porcelain`
shows no unexpected reuse/collision (no framework custody worktree before Batch
4, or only the exact reviewed Batch 4 worktree after its authorized creation);
the custody worktree matches its current frozen step/rollback state; and the
process audit shows no unexpected custody build, test, server, child, verifier,
or reviewer process. A separately reviewed later A0/framework state transition
updates its owning durable record; it is not silently treated as drift.

Before the closure commit exists, ancestry selection yields zero and Batch 0/1
is open. Once the unique commit passes every identity check, it remains valid for
first-parent Batch 2 and later descendants while the fresh live protection
predicate also passes. Missing/multiple closure commits, mismatch, non-first-
parent ancestry, or live protection failure blocks Batch 2 or continuation. The
matching commit metadata is the finite repository control-plane closure record;
no follow-up edit is required to restate its own commit hash. Earlier candidate
or boundary verdicts cannot substitute for this gate.

### Batch 0 Isolation Record And Rollback Points

The following state was recorded before this amendment was authored:

- Canonical Sintra is
  `C:/plms/bsd_licensed/sintra`, branch `master`, at
  `c60c1045dfc2382609eae36afbd637fe274b2871`, matching `origin/master` and
  clean. It is the immutable production baseline and is never an implementation
  worktree. Its rollback point is the same exact revision; no custody commit is
  made there.
- The dedicated custody worktree is
  `C:/plms/bsd_licensed/sintra-managed-child-custody`, branch
  `managed-child-custody-20260711`, created from exact Sintra revision
  `c60c1045dfc2382609eae36afbd637fe274b2871`. The reviewed Batch 0/1 plan is
  committed at `cdd2d4a1cdcb3dca4dbcf575ad3d94ca73a443b5`; the worktree was clean
  before this closure-record edit. The closure-record rollback point is `cdd2d4a...`;
  the whole custody-plan rollback/base remains `c60c104...`. After every later
  accepted atomic step, record the preceding reviewed commit as its rollback.
- The protected A0 worktree is
  `C:/plms/bsd_licensed/sintra-lifecycle-recovery-impl`, branch
  `lifecycle-recovery-slice1`, HEAD
  `099d78a7e1e4e53f875c64d8f9f295f686d0a8fb`. Exactly seven paths are staged:
  `docs/lifecycle_recovery_plan.md`,
  `include/sintra/detail/ipc/rings.h`, `tests/active_tests.txt`,
  `tests/message_ring_autonomous_dead_reader_contract_test.cpp`,
  `tests/message_ring_reentrant_deadlock_contract_test.cpp`,
  `tests/message_ring_reentrant_loss_contract_test.cpp`, and
  `tests/runner/configuration.py`. The exact `git diff --cached --binary`
  artifact is 155101 bytes with SHA-256
  `CEC127D3C964E27268702EFC5654B8FB4AC9804BCA6938B4F5A3F86DCB54C03C`.
  Its rollback point is not a reset target: it is the exact HEAD plus this
  staged index. No custody step may edit, stage, unstage, commit, reset, clean,
  merge, rebase, build, or otherwise touch that worktree or index. The hash and
  seven-path status are checked before and after Batch 0/1 and at every later
  integration boundary until A0 receives an explicit separately reviewed
  disposition.
- Canonical framework is `C:/plms/varinomics/vnm_framework`, branch `master`,
  at `3a69bfc8ccb8c0e42584542c96474113beeefbbb`, with tracked state clean. Its
  protected untracked artifact is
  `mexce_protected_pow_probe.obj`, 3102372 bytes, SHA-256
  `767F8C138768E87FEF1FDBF828B853D5D60D6307D721FA805D3256763B52331E`.
  Neither the canonical framework tree nor that OBJ may be modified or removed.
  Batch 1 creates no framework worktree and makes no framework edit. The
  framework rollback point remains exact revision `3a69bfc8...` until the
  collision-checked Batch 4 worktree is created from it.
- `git worktree list --porcelain` was inspected before the custody worktree was
  used. Historical, salvage, quarry, nested review, and detached worktrees are
  protected: do not prune, remove, reuse, or clean any of them. Before any
  later worktree is created, repeat the porcelain listing and prove both the
  proposed path and branch name unused.

The staged A0 plan contains protected, uncommitted sibling edits. This amendment
was authored from the clean `c60c104` plan and does not copy or silently absorb
those edits. Landing order controls reconciliation: custody may land first while
A0 remains staged and undisposed, or A0 may receive its separate reviewed
disposition and land first. Preserve and hash-check A0 in either case. Only
before the later track with the overlapping plan edit is integrated, a delegated
plan-content reconciliation starts from both frozen diffs and the then-landed
canonical plan. It must preserve both semantic tracks, resolve the single-file
overlap deliberately, re-run independent verification and the full fresh
reviewer pool, and record the resulting commit/hash. A textual merge or custody
commit does not dispose of, supersede, or absorb A0, and A0 disposition is not a
prerequisite for custody to land first.

### Fixed Contract Anchors

All implementation mechanics and later amendments serve these anchors:

1. Durable custody acceptance precedes every path that can create an OS child.
2. Rejected launch means no child can later appear for that request.
3. Accepted launch means Sintra remains responsible through readiness failure,
   timeout, caller cancellation, and public-handle destruction.
4. Readiness is observation after acceptance, never the ownership boundary.
5. Every admitted recovery occurrence has immutable occurrence identity. A
   successful OS creation binds its returned immutable native identity to that
   pre-existing occurrence immediately, before any path can discard or retarget
   it; failed creation records no child and fabricates no native identity.
6. Replacement occurrence state never satisfies predecessor state.
7. Release closes future recovery before retiring already admitted occurrences.
8. Name state, exact publication retirement, exact communication retirement,
   and confirmed OS exit are distinct monotone facts.
9. Communication retirement means no future participation and terminal or
   aborted affected waiters; it does not mean prior delivery or processing.
10. Deadline expiry returns only confirmed facts, changes no ownership, and
    manufactures no milestone.
11. Deadline-facing operations perform no known unbounded nested coordinator
    RPC, local dispatch, registry wait, reader wait, or process wait.
12. Cleanup may continue after the caller deadline under retained Sintra
    ownership.
13. Finalization cannot erase unresolved custody or report successful
    completion while it remains unresolved.
14. Framework owns async-task admission and quiescence until Sintra accepts
    custody and cannot report successful shutdown while a task can still cross
    that boundary.

Deadlines are caller-wait bounds with explicit scheduling tolerance, not hard
real-time scheduling guarantees.

### Smaller State Machine And Impossible States

The contract prevents states instead of wrapping the old uncertainty in a wide
coordinator or result matrix.

```text
launch request
  unaccepted
    +-- reject --------------------------> rejected/no-child-possible
    `-- durable accept ------------------> custody-open
                                              |
                                              +-- admit occurrence N
                                              |     (immutable occurrence is
                                              |      committed before OS-create
                                              |      authority)
                                              |          |
                                              |          +-- create fails --> no-child
                                              |          `-- create returns success
                                              |                `-- bind returned native
                                              |                    identity immediately
                                              |
                                              `-- close recovery ------------> release-closed
                                                                                   |
                                                                                   `-- retire every
                                                                                       admitted N ----> settled

occurrence N facts (monotone and independently authoritative)
  reserved-before-create -> no-child
                         `-> create-success/native-identity-bound
                                  +----> publication retired
                                  +----> communication retired
                                  `----> exact OS exit confirmed
```

Readiness and names are observations attached to an accepted custody or exact
occurrence; they are not state-machine commit edges. A deadline returns a
snapshot of already confirmed facts and creates no edge. Retry and wait observe
or advance the same retained record; they never reconstruct authority from a
raw process ID or name.

This makes the following states unrepresentable by the canonical path: a
rejected request that later creates a child; an OS-created child without a
pre-existing Sintra custody/occurrence record; a successful OS-create result
escaping without its returned native identity bound to that exact occurrence;
a failed create with a fabricated native identity; an accepted child abandoned
by timeout, cancellation, or wrapper destruction; a replacement satisfying an
old occurrence; release cleanup racing with newly admitted recovery; name
absence or OS exit alone satisfying complete release; successful finalization
with an unresolved record; and successful framework shutdown while an admitted
task can still accept custody or do process-capable work.

### Authority Partition And Semantic Operations

Custody is a supervisor that joins facts from their authoritative owners. It is
not one giant owner of coordinator, reader, RPC, barrier, recovery, lifeline,
and OS internals.

- The custody supervisor owns the durable acceptance boundary, the opaque
  logical custody identity, the set of admitted immutable occurrences, recovery
  closure, retained cleanup obligation, and the compact caller-visible
  observation of confirmed facts.
- Spawn/managed-process code owns launch preparation and the native child
  handle or PID/reap identity. It commits the occurrence before authorizing the
  OS-create call. If that call succeeds, it binds the returned native identity
  to the pre-existing occurrence immediately, before returning or entering any
  path that can discard or retarget the result; failed creation records no-child
  without an invented identity. Failed readiness cannot discard a bound native
  identity.
- Recovery owns admission of a new immutable occurrence. It must consult the
  custody record under the recovery-open boundary and cannot admit after release
  closes recovery.
- Coordinator publication, name resolution, reader/communication retirement,
  RPC waiter cancellation, barriers/departure, lifelines, and native process
  observation retain their existing authoritative meanings. Each reports its
  exact-occurrence milestone to custody; custody does not infer one milestone
  from another or expose all internal booleans publicly.
- Framework owns admission and quiescence of its async startup task until the
  task either stops before acceptance or crosses the Sintra acceptance boundary.
  After acceptance it retains the opaque custody identity and uses only the
  canonical Sintra semantic contract. It does not reach through raw Sintra
  globals/maps or regain cleanup authority by name.

The one semantic contract may support these operations without requiring one
giant function:

- launch: atomically reject with no future child or accept durable custody
  before any occurrence can create an OS child;
- observe: report readiness and independently confirmed lifecycle facts for the
  opaque custody/exact occurrence;
- release: close recovery first, then request retirement of every admitted
  occurrence while Sintra retains responsibility;
- retry: idempotently continue the same release after an incomplete deadline,
  without accepting a new occurrence or manufacturing prior milestones; and
- wait: wait only on bounded custody-record notifications until an absolute
  deadline and return the then-confirmed snapshot.

The public shape is compact and source-breaking migration is permitted when the
Batch 3 design is reviewed. No compatibility wrapper, `_v2` API, raw-ID/name
reconstructor, wide drain/RPC/reader/lifeline matrix, or silent degraded success
is allowed.

### Deadline, Release, And Finalization Rules

- Acceptance is the ownership commit. A readiness timeout, failed requested
  publication, cancellation, exception, or destroyed wrapper leaves custody in
  Sintra.
- Deadline-facing launch observation, release, retry, and wait paths use one
  absolute deadline and do not enter a known unbounded synchronous RPC,
  coordinator/local dispatch, spinlocked registry acquisition, reader readiness
  wait, or native process wait. They wait on custody-owned monotone notification
  only to the deadline plus explicit scheduling tolerance.
- Deadline expiry returns an incomplete observation containing only already
  confirmed facts. Cleanup remains admitted and may continue asynchronously;
  expiry never drops ownership, closes the record, or reports an unconfirmed
  milestone.
- Release atomically closes recovery before it requests cleanup. It covers the
  complete set of occurrences admitted before closure and never targets the
  current occupant of a reusable slot by raw ID or name.
- Exact publication retirement, communication retirement, and OS exit are
  separate terminal prerequisites. Communication retirement aborts or
  terminally resolves affected waiters and forbids future participation; it
  makes no claim about delivery or processing of earlier messages.
- Runtime finalization first closes new custody admission and recovery. It may
  wait within an explicit bounded policy or return a typed incomplete/blocking
  outcome, but it cannot delete authoritative state, erase records, or report
  success while any custody or admitted cleanup remains unresolved.

### Relationship To A0 And Existing Slice Order

Managed-child custody and A0 managed transport/fence recovery are independent
production tracks and either may land first. They share only exact occurrence/
generation identity and the rule that communication retirement is not delivery
or processing completion. Custody does not consume staged A0 mechanics, does not
make A0 a prerequisite without new causal evidence, and does not change A0's
production design. If current transport prevents exact communication
retirement, custody remains explicitly incomplete rather than substituting name
absence or OS exit.

The canonical order outside this sibling remains:

```text
A0 evidence -> managed transport successor -> fence successor
            -> full A0 closure -> conditional Slice 6 and later slices
```

Any future evidence of a true semantic dependency first stops both affected
steps, amends this one plan through delegated review, and preserves the fixed
anchors. File overlap alone is not a semantic dependency.

### Future RED/GREEN Matrix

Batch 1 records these gates. Batch 2 implements only the Sintra evidence named
for that batch; R4 remains source diagnosis until Batch 4B.

#### R0: exact dependency contract

- Baseline pair is exact Sintra
  `c60c1045dfc2382609eae36afbd637fe274b2871` plus framework
  `3a69bfc8ccb8c0e42584542c96474113beeefbbb`.
- A framework remote-runtime smoke configure against that explicit local Sintra
  is expected GREEN because current CMake checks only for headers. Compiling the
  target that consumes `vnm_remote_runtime.cpp` is expected RED specifically on
  the absent `Force_process_release_result`,
  `Managed_process::force_release_process()`, `Degraded_shutdown_result`,
  `force_release_known_processes_for_degraded_shutdown()`, and
  `spinlocked_umap::try_scoped()` surfaces. A configure failure, unrelated
  compile failure, timeout, or generic failure is not R0.
- The corrected pair is GREEN only when the framework consumes the actual
  Batch 3 custody surface, every explicit-local, workspace, fallback-fetch,
  install/package, and downstream consumer selection path deterministically
  selects or rejects an exact compatible Sintra, and fallback uses a full
  40-character reviewed Sintra commit pin. Default-branch fetching,
  header-existence-only acceptance, or a capability check for an invented or
  forbidden pre-adoption API spelling is invalid.
- Batch 1 documents this expected RED and creates no framework worktree/edit.
  Batch 4A owns the first truthful pin/capability/build change and the complete
  source-breaking consumer call-site migration after Batch 3 has produced the
  surface actually consumed. A pin with unmigrated scalar/raw/name consumers is
  still R0 RED, not an intermediate pair.

#### R1: hard readiness deadline

Hold the baseline nested synchronous resolution/readiness work causally after
the requested deadline. Baseline RED is the caller remaining trapped beyond the
deadline. Corrected GREEN returns incomplete by the deadline plus scheduling
tolerance while the accepted custody and cleanup responsibility persist.

#### R2: accepted child, readiness not reached

Launch a real child that Sintra accepts but that never reaches the requested
readiness target. Baseline RED is scalar zero with no way to report that the
child existed or remains owned. Corrected GREEN exposes accepted opaque custody
and later exact terminal facts; readiness failure does not close custody.

#### R3: failed readiness before requested-target publication

This is a Sintra-only real-child gate. The managed-process occurrence is
published and its launch nonce, exact occurrence, and native identity are
recorded, while the distinct requested readiness target is causally withheld.
Baseline RED is scalar zero and removal of the relevant name while that
nonce-qualified native child remains alive. Corrected GREEN retains accepted
custody and reports only confirmed occurrence facts until exact retirement/exit.
This is not R5: R3 never permits the requested readiness target to publish.

#### R4: async start versus framework shutdown

Batch 2 performs source diagnosis only and makes no R4 test or shim. Batch 4A
creates the first truthful buildable pair by completing the source-breaking
custody migration at every synchronous and asynchronous framework call site,
including custody-qualified launch observation and release, while deliberately
leaving startup-thread admission/join/detach/reset behavior unchanged. Batch 4B
then adds a real-child causal RED on that exact migrated pair: one hold is before
Sintra acceptance; a second is after acceptance/OS-creation authority. Shutdown
must not report success while the task can cross acceptance or still perform
process-capable work. Corrected 4C GREEN changes only the async admission and
quiescence lifecycle (plus same-atom orphan removal); custody consumption is
already canonical in 4A. If a lawful R4 RED cannot be produced after 4A without
shims, fakes, invented APIs, or oracle substitution, return architecture RED and
stop.

#### R5: name disappears after actual requested publication

Allow the requested target to publish, then causally remove its name before
later exact publication/departure, communication, or OS retirement completes.
Baseline RED is name absence being treated as release while the exact native
child or another required retirement fact remains nonterminal. Corrected GREEN
keeps release incomplete. Unlike R3, this gate proves actual requested
publication followed by name removal.

#### R6: recovery occurrence isolation

Admit occurrence `n`, recover as immutable occurrence `n+1`, and hold a
predecessor milestone. Baseline RED demonstrates reusable-slot/raw identity can
let replacement state stand in for `n`. Corrected GREEN never lets `n+1`
satisfy `n`; release closes recovery first and covers every occurrence admitted
before closure.

#### R7: adverse coordinator/reader cleanup

Use the existing test-only coordinator seam
`k_stage_unpublish_pre_barrier_collection` in
`Coordinator::unpublish_transceiver()`. Launch a real lifeline-disabled child
with a nonce-qualified exact occurrence and withhold its requested readiness
target. Let failed-readiness cleanup enter synchronous normal unpublish and hold
it at that seam: current source has already erased the reverse name and process
registry entries and set draining, but has not collected barrier completions,
stopped the exact reader, emitted final lifecycle/unpublish notifications, or
confirmed native exit. Because this child is lifeline-disabled, failed-start
cleanup skips its lifeline-release block; no lifeline entry existed for this
occurrence and none remains at the seam. The coordinator's later
`release_lifeline()` lookup therefore finds no entry/returns false. That is
authoritative absence, not a successful lifeline release and not a release
milestone.

Exact baseline RED is the public spawn caller remaining blocked beyond its
requested deadline plus tolerance inside that held cleanup call, with the seam
marker complete, the exact occurrence/native child still alive, the name absent,
and the exact reader/communication-retirement state still nonterminal. This is
a causal deadline overrun and loss of a caller-visible authoritative cleanup
result: the scalar API cannot return the unresolved responsibility while the
cleanup stack is held. A generic watchdog timeout, name absence, stopped
communication, native exit alone, or a hold reached before the named seam is
invalid. Corrected behavior returns incomplete by the deadline plus tolerance,
retains responsibility, aborts/terminally resolves affected waiters only when
communication retirement is authoritative, and later completes without
residue after the seam is released.

#### R8: finalization with unresolved custody

Batch 2 adds one minimal semantic test-only runtime seam immediately after the
admission-guarded `Managed_process::spawn_swarm_process()` returns a successful
`Spawn_result` and the admission mutex is released, but before the public
readiness wait begins. This is current-source accepted-like state, not a future
custody API: the result says `success`/`os_process_created`, contains the exact
PID/handle, and current managed-process state has the occurrence reader,
spawn/recovery bookkeeping, and coordinator registry work. The child is
lifeline-disabled, so this state contains no lifeline entry and claims no
lifeline-release fact.

At the seam, hold the root spawn caller. The real lifeline-disabled child
publishes its managed-process occurrence, causally calls the existing
begin-process-draining path so coordinator finalization need not hang on its
draining wait, withholds the requested readiness target, and remains alive on a
native latch. A second root thread invokes current `detail::finalize()`, which
closes teardown admission. Exact baseline RED is `finalize()` returning `true`,
deleting/resetting `s_mproc` and restoring lifecycle state to idle while the
held spawn caller still owns the successful current `Spawn_result` and the
nonce/occurrence-qualified native child remains alive. After the seam releases,
the readiness failure reaches current `cleanup_failed_wait()` with `!s_mproc`,
logs that cleanup cannot run, returns scalar zero, and still has no confirmed
native exit. Finalization success/state reset while old process-capable work can
resume is the required false-success/erase/unsafe-progress manifestation.

A hang in draining/finalization, a generic timeout, a child that never reached
the spawn seam, synthetic registry mutation, or a child already exited is
invalid R8 evidence. If the real child cannot make the exact source-supported
draining/seam choreography executable on `c60c104`, R8 returns architecture RED:
stop Batch 2, amend this plan, and assign the unresolved finalization proof to a
named pre-Batch-3 evidence successor before any custody production code; do not
invent a custody handle or accept a weaker oracle. Corrected behavior closes
admission and recovery, admits no new occurrence, retains the record, and cannot
erase it, reset to idle, or report successful finalization until it settles.

Every behavioral gate uses a real child, unique launch nonce, exact occurrence,
native process identity, causal markers/latches, and native exit observation.
Timeout is a watchdog only. Name absence, synthetic registry mutation,
cancellation, generic timeout, crash, clean exit, stopped communication, or
state reset alone is an invalid oracle. Each run records commands, exit codes,
causal fields, surviving process audit, and repository state.

### Six Batches

Each atomic step freezes its owner files, adopted contract, expected gate,
prohibited adjacent scope, and rollback commit before delegation. Later-batch
file families are boundaries, not permission to touch every listed file.

#### Batch 0: protect staged A0 evidence

- Owner is orchestration state only; no implementation file is writable.
- Gate is exact branch/base/status/worktree collision evidence plus A0's seven
  staged paths, 155101-byte binary patch, and SHA-256 above before and after the
  batch. The canonical trees and protected OBJ remain unchanged.
- Prohibited scope is every edit, build, test, clean, reset, commit, merge,
  rebase, or index operation in A0/canonical/historical worktrees.
- Rollback is removal only of a newly created, still-clean dedicated custody
  worktree/branch after proving its resolved path; never reset a protected tree.

#### Batch 1: one-plan amendment and exact-pair specification

- Writable owner file is only `docs/lifecycle_recovery_plan.md` in the
  dedicated custody worktree.
- Gate is documentation/static verification, frozen one-file diff and hash,
  unchanged A0 hash/status, then a maximum fresh independent review round with
  unanimous unqualified GREEN. No build is applicable to this doc-only step.
- Prohibited scope is production lifecycle behavior, child-custody RED tests,
  framework CMake/CI/runtime edits, an invented API spelling/capability probe,
  or a second plan.
- Rollback is exact custody base `c60c104...` until the reviewed narrow plan
  commit exists; the resulting plan commit becomes Batch 2's rollback point.

#### Batch 2: deterministic baseline RED evidence only

- Owner families are new custody contract test sources, their test registration
  and runner configuration, and the smallest semantic test-only causal seams
  under `SINTRA_ENABLE_TEST_HOOKS`. The atomic step must enumerate exact files
  before editing.
- Implement real-child R1, R2, R3, R5, R6, R7, and R8 gates as separable atomic
  steps where write sets or causal oracles differ. R0 runs as the exact-pair
  configure-GREEN/compile-RED dependency proof. R4 is source diagnosis only.
- Expected gate is the exact causal baseline result for each named RED, not a
  timeout or generic failure. An independent verifier records commands, exit
  codes, causal markers, survivors, and repository state.
- Prohibited scope is all production lifecycle semantics, framework behavior,
  compatibility shims, synthetic registry oracles, and A0 mechanics.
- Rollback is the reviewed Batch 1 commit; every accepted RED step advances a
  new recorded reviewed commit. Any gate that cannot produce its intended
  source-supported RED stops production and requires a reviewed plan amendment
  or explicit dropped/rescoped gate.

#### Batch 3: Sintra custody implementation and cleanup absorption

- Owner families are the canonical public/runtime launch surface and the
  minimum managed-process, recovery, coordinator/reader notification,
  lifeline/native-process, and finalization internals required to implement the
  supervisor and join authoritative facts. Exact files and disjoint write sets
  are frozen per atomic step.
- Add the one canonical custody contract and supervisor/records; commit durable
  custody and immutable occurrence identity before OS child creation authority;
  on successful creation bind the returned native identity immediately before
  any path can discard/retarget it, and record failed creation as no-child;
  implement bounded observation/release/retry/wait; close recovery before
  cleanup; retain unresolved records through finalization.
- Absorb failed-start cleanup into the authoritative custody owner in the same
  atomic migration that makes it redundant. Delete the old scalar-result/
  discarded-cleanup composition, duplicate cleanup authority, and every newly
  orphaned helper/test path in that same commit after call-site proof. Run an
  exact symbol/call-site orphan sweep; do not defer deletion.
- Expected Sintra GREEN is R1/R2/R3/R5/R6/R7/R8 plus focused existing launch,
  recovery, lifeline, publication/departure, communication, RPC, and
  finalization gates selected from the changed surface in Debug and Release.
  This establishes the Sintra R3/release contract only; the framework-consumer
  R3/release cutover and GREEN belong to 4A. R0 remains RED for the archived
  pair; R4 still awaits the complete runnable 4A pair.
- Prohibited scope is framework migration, A0 transport/fence behavior,
  historical salvage primitives, raw-ID/name authority, a public substep
  matrix, or unbounded deadline-facing work.
- Rollback is the reviewed Batch 2 evidence commit; each accepted implementation
  atom advances only after independent verification and unanimous review.

#### Batch 4: framework custody cutover, R4 evidence, async repair, and deletion

Create a separate framework worktree from exact `3a69bfc...` only after a fresh
porcelain collision check. Suggested path/branch are
`C:/plms/varinomics/vnm_framework-managed-child-custody` and
`managed-child-custody-dependency-20260711`; change them if either is occupied.
Never use the canonical framework worktree or protected OBJ.

1. **4A complete source-breaking custody cutover and dependency
   decontamination.** Owner families are dependency selection/CMake/package/
   consumer configuration; `cpp/remote_ui_runtime/vnm_remote_runtime.*`;
   `cpp/remote_ui_runtime/vnm_remote_runtime_internal.h` (migrate its surviving
   declarations or delete it after call-site proof);
   `cpp/remote_ui_runtime/vnm_hosted_worker_session.*`;
   `cpp/remote_ui_runtime/vnm_hosted_worker_host_lifecycle.*`; their focused
   session/host-lifecycle/remote-runtime tests; and no other framework files
   unless frozen call-site inventory proves them consumers. Pin a full
   40-character exact reviewed Batch 3 Sintra commit and check only the actual
   custody surface consumed. In the same atomic source-breaking cutover:

   - migrate every synchronous and asynchronous launch result, stored session
     state, failure cleanup, release, observation, retry/wait, and shutdown-side
     call site from scalar process count/raw process ID/name authority to the
     opaque custody identity and canonical custody observations;
   - make the framework-consumer form of R3/release GREEN: failed requested
     readiness retains custody, framework cleanup/release targets that custody,
     and no name/raw-ID fact manufactures release;
   - delete every phantom consumer of the five absent salvage surfaces, raw
     Sintra reach-through, raw-ID/name cleanup authority, name-polling success,
     duplicate result matrix, obsolete helper/test/CMake path, and newly
     orphaned declaration in the same atom, including migration or deletion of
     `vnm_hosted_worker_host_lifecycle.*` and
     `vnm_remote_runtime_internal.h` as their exact call-site proof requires;
     and
   - prove deterministic explicit-local, workspace, fallback-fetch,
     install/package, and downstream-consumer selection and compilation.

   4A creates the first truthful buildable pair. It may reshape data carried by
   the startup task only as necessary to use custody, but intentionally preserves
   the existing async admission/quiescence defect: do not change startup-thread
   admission closure, wait/join timing, detach behavior, process-state reset, or
   the condition under which shutdown reports success. There is no temporary
   scalar/raw/name adapter, compatibility bridge, or intermediate unbuildable
   commit. Exact 4A gates are R0 corrected-pair GREEN, migrated framework R3/
   release GREEN, focused synchronous and async call-site compilation/behavior
   GREEN, and source/diff proof that the named async lifecycle control flow is
   unchanged and remains eligible for R4 RED.

2. **4B R4 evidence on the exact 4A pair.** Owner families are new focused
   framework real-child test support, registration, and minimal semantic causal
   seams only. With 4A custody use and deletion frozen and async admission/join/
   detach/reset behavior unchanged, add the pre-acceptance and post-acceptance/
   OS-creation holds and record exact R4 RED. No custody adapter, deleted raw/
   name authority, or async production fix may ride with the evidence. If lawful
   RED requires shims, fakes, restored authority, or oracle substitution,
   architecture RED stops the program for diagnosis and reviewed plan amendment.

3. **4C async admission/quiescence repair and R4-orphan deletion.** Owner
   families are `cpp/remote_ui_runtime/vnm_hosted_worker_session.*`, only the
   already custody-migrated remote-runtime/host-lifecycle files whose exact
   call sites require async-lifecycle adjustment, the permanent R4 test, and
   their registration. Make startup-task admission closure and join/quiescence
   authoritative; after acceptance the already-carried custody remains retained
   and settled; prevent detach/reset/successful shutdown while the task can cross
   acceptance or perform process-capable work. Do not repeat or defer custody
   call-site migration from 4A. In this same atom delete every R4 causal seam,
   test adapter, lifecycle helper, declaration, test branch, and build entry
   newly orphaned by the repair; retain a seam only when the permanent R4 GREEN
   regression still calls it and its semantic test-only scope is independently
   accepted. Exact 4C gate is R4 GREEN plus focused async shutdown/session and
   survivor/clean-state gates.

Batch 4 cumulative GREEN is 4A's R0 corrected-pair and framework R3/release
GREEN, 4B's exact R4 RED evidence, 4C's R4 GREEN, and all migrated focused
framework/package/consumer gates. Each 4A/4B/4C atom has its own rollback pair
of exact Sintra/framework commits, verifier, frozen diff, and unanimous review:
4A rolls back to exact framework `3a69bfc...` plus the reviewed Batch 3 Sintra
commit; 4B rolls back to the reviewed 4A pair; 4C rolls back to the reviewed 4B
pair. No compatibility path may preserve deleted authority. Prohibited scope is
A0 transport/fence production, changes in either canonical worktree, any async
join/detach/reset fix in 4A or 4B, any custody/raw/name compatibility adapter,
an unbuildable intermediate commit, or retention of an orphan for later cleanup.

#### Batch 5: cumulative gates and paired release

- Owner families are only test/CI roster/workflow/package configuration and
  this plan's closure record unless a failure reopens the owning earlier batch.
- Run focused Debug/Release gates, supported-platform targeted CI, complete
  active rosters where selected, sanitizer/race lanes where supported,
  install/package and downstream consumer builds, exact paired revision checks,
  native survivor audits, and clean-state audits. A failure is not patched in
  Batch 5: delegate diagnosis/remediation to the owning batch, rerun its local
  gate, and repeat its full fresh unanimous review before repush.
- After implementation is otherwise complete, separately delegate a final
  full-suite enablement change. Independently verify it, freeze its exact diff,
  obtain the maximum fresh reviewer pool's unanimous GREEN, commit and push it,
  then require full paired supported-platform CI/package/consumer closure. No
  earlier focused or background run substitutes for this final gate.
- Record final branch/commit hashes, exact paired package revisions, CI run IDs,
  test counts, review verdicts, deleted paths/symbols, residual risks, rollback
  points, clean statuses, A0 hash/protected state and any separately reviewed
  disposition that already exists, and proof that no process, thread, custody
  record, build, helper, reviewer, or CI session remains unresolved. Custody
  landing first does not manufacture or require an A0 disposition.
- Prohibited scope is new lifecycle semantics, opportunistic production repair,
  weakening a confirmed oracle, treating a background run as a blocking gate,
  or bypassing an owning batch's remediation/review loop. The rollback point is
  the last exact unanimously reviewed Sintra/framework commit pair before each
  CI/packaging atom; the final release pair is recorded only after all closure
  gates pass.

### CI Selection And Push Policy

Commits, pushes, and PRs are authorized after the applicable frozen-candidate
verification and unanimous review. Intermediate commits/pushes/PRs may leave the
full roster enabled; such runs are non-gating background evidence and every
observed failure is still reported and assigned. They do not permit advancing a
blocked atomic step.

Whenever pre-final CI is deliberately observed as a blocking gate, select only
the exact union of:

1. tests failing in the preceding 72-hour window; and
2. tests newly introduced by the atomic step.

The delegated CI verifier records the UTC query time and cutoff, repository and
branch/workflow filters, query command/API, source run IDs and attempts, exact
failed test identities extracted from logs/artifacts, de-duplicated selected
roster, new-test list, triggered commit/run IDs, and all outcomes. Selection
must be evidence-based; a guessed historical roster is invalid. Every observed
failure is reported, causally diagnosed, and remediated through the owning
atomic loop. The final Batch 5 full-suite enablement and closure are mandatory
even if an intermediate background full-roster run happened to pass.

### Mandatory Atomic-Step And Review Protocol

Every plan edit, test/evidence change, production change, dependency migration,
deletion, remediation, build, test run, CI change, and substantive verification
uses this loop. All workers and reviewers are `gpt-5.6-sol`, xhigh reasoning,
priority service tier.

1. Freeze one atomic step: exact owner files, adopted contract, expected named
   RED/GREEN, prohibited adjacent scope, and rollback commit(s).
2. Delegate edits to one or more workers only when write sets are genuinely
   disjoint. Workers know other work exists and never revert unrelated/user
   changes.
3. Delegate verification to a worker other than every author. Record exact
   commands, exit codes, causal evidence, survivors, and repository state. A
   timeout or generic failure is never accepted as the named result.
4. Freeze exact candidate commit/diff, file list, evidence, and hashes. No edit
   occurs while it is under review.
5. Close completed workers and use the maximum available number of fresh
   independent reviewers. Cover correctness, concurrency/lifecycle, oracle
   quality, cross-platform behavior, governance/deletion, and architecture.
   No author reviews their own change.
6. Require every reviewer to return unqualified `GREEN`. `AMBER`, conditional
   green, required changes, unresolved questions, or material suggestions are
   non-GREEN.
7. On any non-GREEN, stop and delegate remediation. Re-run the exact independent
   verification, freeze a new candidate, and repeat a full maximum-fresh review
   round. Findings are not dismissed or patched by the orchestrator.
8. Commit/advance only after closure. Push/CI follows the reviewed frozen local
   commit. Any CI failure reopens the same atom for delegated diagnosis,
   remediation, local rerun, and another full unanimous review before repush.

At each batch boundary, freeze the entire cumulative batch and run a new maximum
fresh reviewer convergence round over plan conformance, evidence, deletions,
state, and next-batch preconditions. Per-step GREEN does not imply batch GREEN.
Review findings distinguish current blockers from risks owned by a named future
batch and from notes; later mechanics are not demanded before their predecessor
evidence exists. Every concrete build, test, crash, leak, or other failure a
reviewer observes is nevertheless a finding with its identity, provenance
(introduced, perpetuated, or exposed), and delegated remediation path; multiple
failures are counted prominently and grouped. No failure is omitted as merely
pre-existing or outside the changed diff.

At least one fresh reviewer in every round is the architecture lane. It receives
this durable plan, the online validation, cumulative frozen diff, current
evidence, and A0/neighbor state, and answers explicitly:

1. Does custody acceptance precede every possible OS child creation?
2. Is custody retained through readiness failure, timeout, cancellation,
   wrapper destruction, recovery, and finalization?
3. Are logical custody and immutable occurrence distinct, and can replacement
   state satisfy any predecessor fact?
4. Does release close recovery before cleanup and cover every admitted
   occurrence without raw ID/name targeting?
5. Are name, publication retirement, communication retirement, and exact OS
   exit distinct authoritative facts?
6. Does every deadline path avoid known unbounded nested work and retain
   ownership after timeout?
7. Can finalization or framework shutdown report success while custody or an
   async task remains capable of process work?
8. Is communication retirement conflated with delivery/processing, or has an
   unjustified A0 dependency appeared?
9. Has a duplicate authority, compatibility path, result matrix, raw reach-
   through, or undeleted obsolete path appeared?
10. Does the change match this plan; if different, is there new causal evidence
    and a reviewed material amendment rather than drift?
11. Has new state/mechanism made an invalid state representable that the
    architecture intended to eliminate?
12. Do the next step and batch order still hold, or has evidence invalidated a
    prerequisite?

An architecture RED stops all implementation. Delegate diagnosis and any plan
amendment first, then freeze and obtain a full maximum-reviewer unanimous GREEN
before code resumes.

### Same-Batch Deletion And Orphan Proof

Every migration atom inventories all callers before editing and repeats exact
symbol/file searches after editing. The commit that removes the final caller
also deletes the newly orphaned helper, test, documentation claim, build entry,
configuration branch, generated/derived artifact, and compatibility path.
False-hypothesis artifacts are deleted rather than retained as speculative
utilities. If a required deletion cannot share the atom, split the atom before
implementation; do not defer it. Batch 3 owns failed-start cleanup absorption;
Batch 4A owns the complete framework custody call-site cutover plus raw/name/
salvage, obsolete lifecycle-helper/test, and dependency-path deletion; Batch 4C
owns every R4 seam/adapter/helper newly orphaned by async repair.

### Non-Goals

- Do not restore the historical salvage implementation or any of its forbidden
  symbols, and do not use historical branches/patches except as scenario quarry.
- Do not create a monolithic custody owner for all subsystem internals or a
  public matrix of their substeps.
- Do not preserve old and new public launch/release authority through aliases or
  compatibility wrappers.
- Do not promise hard real-time deadlines, message delivery/processing from
  communication retirement, or release from name absence/OS exit alone.
- Do not make framework async admission Sintra-owned, expose raw internal maps,
  or let the framework reconstruct custody from process ID/name.
- Do not make A0 a custody prerequisite from overlapping files alone, alter A0
  mechanics, or silently merge its staged plan.
- Do not implement RED tests or production behavior in Batch 1, and do not
  pre-adopt API spelling through a dependency capability check.
- Do not create a temporary scalar/raw-ID/name custody adapter, compatibility
  bridge, or unbuildable pin-only framework transition between Batch 3 and 4A.

### Custody Stop Conditions

Stop before commit/advance and amend through the full loop if:

- A0's staged path set, 155101-byte size, or SHA-256 changes;
- any work is proposed in a canonical, protected A0, or historical worktree, or
  the protected framework OBJ changes;
- a second canonical lifecycle document is created;
- custody becomes a giant owner of coordinator, reader, RPC, barrier, recovery,
  and OS internals instead of a supervisor joining authoritative facts;
- acceptance can follow OS creation, rejected launch can later create a child,
  or accepted ownership can be dropped by timeout/cancellation/destruction;
- raw process ID/name can reconstruct custody or replacement state can satisfy
  predecessor facts;
- release does not close recovery before cleanup or omit an admitted occurrence;
- deadline expiry manufactures success, enters known unbounded nested work, or
  drops retained ownership;
- finalization can erase unresolved custody or report success;
- framework async admission is assigned wholly to Sintra or framework shutdown
  can succeed while task/process-capable work remains;
- communication retirement is equated with delivery/processing or custody gains
  an unjustified A0 dependency;
- the dependency gate assumes a not-yet-adopted/forbidden spelling, default
  branch, or header-only compatibility;
- Batch 1 adds runtime behavior/tests/framework edits; Batch 2 adds production;
  or historical salvage is imported rather than used as evidence;
- R3 permits requested-target publication, R5 omits it, 4A fails to migrate all
  synchronous/asynchronous custody consumers and delete raw/name/salvage
  authority in the same buildable atom, 4A changes async join/detach/reset
  behavior, 4B runs on anything other than the exact migrated 4A pair, 4C
  repeats deferred custody migration or leaves a newly orphaned R4 path, or any
  R4 evidence/fix begins before truthful 4A closure; or
- any reviewer returns non-GREEN, an architecture question is unresolved, or
  the required independent verification/review evidence is missing.

## Implementation Slices

Each slice has one objective, one focused test or existing test target, and no
speculative public/test helper surface. If a slice needs broad new state, stop
and split it.

For every conditional slice, record a durable yes/no decision in this file
before skipping it: whether the selected baseline exhibits the bug, what
evidence was checked, and which focused gate covers the decision.

### Slice 0: CI And Test Discipline

- As of the user-requested restoration in `8f782bf`, blocking recovery CI uses
  the full top-level non-manual selection in `tests/active_tests.txt`. The
  earlier `bf3ba2e` active-only recent-failures roster is historical and no
  longer defines the blocking gate.
- Full selection restores test breadth, not historical stress depth. The
  checked-in iteration counts remain the current source of truth, but this gate
  does not claim that every historical stress multiplier, repetition count, or
  runtime duration has been restored.
- Before claiming a slice green, the full-selection blocking CI must compile and
  run on Linux, macOS, Windows, and FreeBSD unless a recorded user instruction
  or CI outage narrows that requirement.
- Blocking stress-test jobs should use a global `run_tests.py --time-budget`
  cap so repetition-heavy full-selection runs cannot turn CI into an unbounded
  wait.
  Budget exhaustion is green only after each selected test has at least one
  passing run; `did_not_run` does not satisfy the budget cap.
- `active_tests.txt` entries missing from the build are fatal runner errors.
  The full-selection gate is invalid if it silently runs only the subset of
  active tests whose binaries happened to be present.
- The post-`ceafde0` remediation owns only the test and CI corrections recorded
  below. Production IPC and runtime headers are outside that repair.

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

Slice 2 decision and implementation record, 2026-07-09:

- Initial source audit confirmed the simple admission gate is still the only
  lifecycle admission primitive: `s_teardown_admission_mutex` plus
  `s_teardown_admission_closed`. No `288139f` runtime-work,
  finalize-blocker, pending-owner, or lifecycle-exchange machinery is needed.
- The first six-Codex scope review split: four reviewers returned no-code or
  doc-only closure, while two reviewers found source-confirmed residual risks.
  Claude's first Slice 2 review returned `GREEN_DOC_ONLY`, but the local
  source audit preserved the two red claims for convergence.
- The targeted convergence round found two concrete Slice 2 issues. Three
  Codex reviewers returned `GREEN_FIX_BOTH`, one `GREEN_FIX_A_ONLY`, one
  `GREEN_DOC_ONLY`, and one `RED_NEEDS_RED_GATE_FIRST`. Claude returned
  `RED_NEEDS_RED_GATE_FIRST`. The orchestrator chose the red-gate-first path
  under governance rule 12.
- Finding A: `create_external_process_invitation()` checked `s_mproc/s_coord`
  before `s_teardown_admission_mutex`, then used them after a possible
  finalize/reopen interleaving. The active
  `external_process_invitation_lifecycle_negative_test` now pauses creation at
  a test-only pre-admission hook, finalizes the runtime, resumes creation, and
  asserts that coordinator reservation is not reached. Red evidence:
  `build\slice1-focused-mingw-bigobj\slice2_a_invitation_toc_red_run.txt`
  exited `1` on current production with the reservation-reached assertion.
  Production fix: re-check active coordinator/runtime state under the admission
  mutex before the closed check and before reservation. Green evidence:
  `slice2_a_invitation_toc_green_run.txt` and
  `slice2_a_invitation_toc_green_rerun_after_b.txt` exited `0`.
- Finding B: default recovery could run inline from
  `Coordinator::unpublish_transceiver()` while `m_publish_mutex` was still
  held, then successful recovery spawn re-acquired `m_publish_mutex`. The new
  active `recovery_unpublish_deadlock_test` seeds one recoverable fake process
  with a cached spawn command and calls coordinator unpublish. Red evidence:
  `build\slice1-focused-mingw-bigobj\slice2_b_recovery_unpublish_red_run.txt`
  exited `1` with the ranked publish-mutex assertion at
  `coordinator.h:158`. Production fix: route the default recovery spawn through
  the existing `m_recovery_threads` path, matching custom recovery runners and
  letting unpublish release coordinator locks before spawn. Green evidence:
  `slice2_b_recovery_unpublish_green_run.txt` exited `0`.
- Focused CI roster impact: add only `recovery_unpublish_deadlock_test 1` to
  `tests/active_tests.txt`. Do not add broad recovery suites to blocking CI for
  this slice.
- Implementation review, 2026-07-09: six xhigh Codex reviewers returned green
  after the stale recovery docs were updated. The review covered the production
  diff, local red/green gates, active-test scope, lock ordering, and absence of
  4B lifecycle scaffolding.
- Focused platform CI, 2026-07-09: commit `577a6cf` passed the shortened
  active-test gate on all four platforms. Windows passed on the original run
  `29018511603`; Linux `29018511468`, FreeBSD `29018511306`, and macOS
  `29018511519` passed on attempt 2 after the original jobs were cancelled
  without failed logs. Slice 2 is closed.

### Slice 3: RPC Terminal Cleanup

Durable drop decision, 2026-07-09:

- Drop Slice 3 production work on this recovery branch. Six xhigh Codex
  convergence reviewers and Claude returned green on the drop decision after
  the first round split between narrow enum polish and no-production findings.
- No current-HEAD RPC terminal cleanup bug was reproduced. HEAD already has
  `detail::Rpc_completion_state`, and sync RPC already routes
  `rpc_impl() -> handle.get() -> wait()/state() -> cleanup_rpc_state_if_needed()`
  before returning or throwing.
- The remaining `Outstanding_rpc_control` booleans are representation polish,
  not red evidence. They are guarded by `keep_waiting_mutex`; reply, exception,
  abandon, and teardown unblock paths test the waiting latch before committing a
  terminal outcome.
- Anchor correction: `436e7f2` is doc-only. The real 4A RPC terminal-state
  patch is `a5f7f72`, and it is not self-contained because it also carries
  runtime blocker/finalize machinery.
- Exclude `d373c56` on this baseline. Its `Terminal_cleanup_guard` depends on
  repair-line helper shape absent from HEAD and is redundant with current
  `handle.get()` cleanup.
- Exclude `Finalize_blocker_kind`, `rpc_cleanup_busy`,
  `append_rpc_cleanup_blockers`, shared-ownership reshaping of
  `s_outstanding_rpcs`, and all 4B lifecycle/helper imports from Slice 3.
- Reopen only with deterministic current-HEAD red evidence showing a real RPC
  terminal-state or cleanup correctness failure. Otherwise treat enum collapse
  as later non-recovery polish, not as part of this recovery line.

### Slice 4: Finalize And Drain

Durable drop decision, 2026-07-09:

- Drop Slice 4 production work on this recovery branch. The first review round
  split on narrowed 6A0, then six xhigh Codex convergence reviewers and Claude
  unanimously returned DROP.
- Drop `5062001`'s `s_finalize_drain_prepared` and explicit teardown admission
  registration. That shape belongs to the old Admission_boundary/finalize-result
  line and no current-HEAD red evidence shows the linear finalize path needs it.
- Do not port `5062001`'s external-attached drain-wait exclusion. HEAD already
  handles external peers in the supported shutdown path through
  `group_has_non_external_peer()`, `shutdown_coordinator_drain_wait()`, and
  `begin_shutdown()` marking external-attached processes draining.
- Drop the 6A0 cleanup/removal predicate from `55918b4` on this baseline.
  Current source, tests, and docs define `wait_for_all_draining()` as the
  draining-bit predicate. Cleanup/removal semantics are owned by
  `shutdown_coordinator_drain_wait()` before finalize on the supported
  collective shutdown path.
- Do not add a test-only red gate that merely rewrites
  `drain_wake_coordination_test` from "draining bit satisfies wait" to
  "cleanup/removal satisfies wait"; that would mint a new oracle, not reproduce
  a current bug.
- Exclude `330a29f` from Slice 4. Its acknowledged self-unpublish producer is
  conditional on keeping 6A0, and the 6A0 predicate is dropped here.
- Exclude `Drain_wait_snapshot`, `Drain_wait_result`,
  `wait_for_all_draining_result`, `drain_waiter_live`, finalize blocker/result
  plumbing, drain attribution maps, `Admission_boundary`, and all runtime-work
  or pending-owner scaffolding from this recovery line.
- Reopen only with deterministic current-HEAD evidence that a supported public
  path finalizes while a peer is draining-but-not-removed and that this causes
  observable harm, such as lost messages, hang, corruption, or torn lifeline
  behavior. A retained synthetic candidate alone is not enough.

### Slice 5: Shutdown/User-Barrier Membership

Red-gate decision, 2026-07-09:

- Six xhigh Codex reviewers and Claude converged on one next step: no production
  work yet; add only a focused test-only gate adapted from
  `ae7fed3`'s `shutdown_user_barrier_departure_test`.
- Valid red evidence: one process enters collective `shutdown()` and waits in a
  Sintra internal shutdown barrier; remaining live peers then cannot complete a
  normal user barrier because the shutdown participant is still counted in
  user-barrier membership.
- The gate must be re-derived against current HEAD using existing observability.
  Do not add a production test hook, `begin_collective_shutdown`, a new state
  array, or any part of the production fix to make the red observable.
- If the gate passes, fails before proving the shutdown-participant/user-barrier
  membership cause, or only reports a generic timeout, durably drop Slice 5.
- If the gate fails for the intended reason, run a fresh architecture review
  before production work. First attempt the smallest internal fix; add a distinct
  collective-shutdown state or internal RPC only if the reproduced red proves the
  existing draining state plus barrier classification cannot represent the case.
- Treat stale process-slot reset and forged `begin_collective_shutdown`
  rejection as follow-on green coverage only if production adds the corresponding
  internal state/RPC. They are not standalone current-HEAD red gates.
- Exclude `tests/admission_process_entry_contract_test.cpp`, group membership
  authority, public API changes, `Admission_boundary`, runtime-work counters,
  pending-owner state, finalize/drain result machinery, and broad `ae7fed3`
  cherry-picking from Slice 5.

Production authorization, 2026-07-09:

- The focused gate failed red for the intended reason: an early collective
  shutdown participant could keep live peers blocked in a user
  `processing_fence_t` barrier.
- Fresh architecture review authorized only the minimal production shape:
  a private per-process collective-shutdown bit, one internal
  `begin_collective_shutdown(process_iid)` RPC, user-barrier-only membership
  filtering, and shared `_sintra_processing_phase/` internal-barrier
  classification.
- Production remains bounded by the original exclusions above. Do not add
  public APIs, `Admission_boundary`, pending-owner state, result/finalize
  matrices, or broad lifecycle framework code.
- The open implementation gate is local focused green, six xhigh Codex
  implementation review, Claude implementation review, then shortened active
  CI. The first implementation review round found narrow blockers in sender
  validation, the admission-closed/state-idle barrier guard, external-attach
  reset ordering, and this durable plan record; those must be closed before
  Slice 5 can be accepted.
- The second implementation review round found two additional narrow blockers:
  RPC-context validation had to require the actual `begin_collective_shutdown`
  reserved message id, and the pre-barrier announcement had to preserve
  degraded teardown on `rpc_cancelled`/`rpc_unavailable`. Those must also be
  closed before Slice 5 can be accepted.

Closure, 2026-07-09:

- Slice 5 is closed at `f95cf6e`. The implementation received a green third
  review round from six xhigh Codex reviewers and Claude.
- Local focused gates passed: `git diff --cached --check`,
  `sintra_shutdown_user_barrier_departure_test` build and direct executable,
  and `sintra_ring_abi_fingerprint_test` build and direct executable.
- Shortened active CI passed on Linux, FreeBSD, Windows, and macOS for
  `f95cf6e`.

#### Test/CI recovery remediation after `8f782bf..ceafde0`

- `8f782bf` restored the full top-level test selection and exposed a pre-existing
  iteration-handoff race in the backlog fence test. It did not expose a
  production runtime failure or a Slice 5 regression.
- `ea20e30` added a per-iteration start rendezvous before workers emit the next
  burst and before the coordinator begins its first-marker wait. That ordering
  directly closes the observed stale-iteration handoff instead of masking it.
- `ceafde0` reduced the internal rounds from 64 to 16 only to fit the generic
  30-second macOS runner timeout. It did not change the oracle or fix another
  race.
- Four independent reviewers accepted that causal account: the failure belonged
  to the old test handoff, the `ea20e30` rendezvous fixes the exact interleaving,
  and neither production behavior nor the Slice 5 implementation was implicated.
  They also agreed that the active oracle waits for delayed handler completion,
  which is processing-fence behavior under the documented contract, not a
  delivery-fence oracle.
- The same review found three CI integrity gaps. Linux, macOS, and Windows branch
  builds could compile with tests disabled while their reusable stress workflows
  independently decided to run tests; all three stress workflows published the
  same external status context; and coverage discarded the runner failure with
  a bare `|| true`. The canonical plan also had no record of
  `8f782bf..ceafde0`.
- The bounded remediation reclassifies and renames the active test as a
  processing-fence backlog test, keeps the `ea20e30` rendezvous, restores 64
  internal rounds behind a canonical 90-second timeout override, and removes the
  stale manual delivery-fence duplicate as a false-oracle artifact. It also
  makes test compilation unconditional in the Linux, macOS, and Windows build
  workflows, gives their external stress statuses distinct contexts, and
  propagates the captured coverage runner result only after coverage generation
  and upload steps have run.
- Local acceptance requires `git diff --check`, focused Python and workflow
  static checks, isolated configure/build of the renamed target, and at least one
  direct successful execution of its full 64 internal rounds. The roster,
  timeout lookup, manual CMake list, and documentation references must all name
  only the surviving processing-fence test.
- Local remediation evidence, 2026-07-09: `git diff --check`, Python syntax,
  active-roster/timeout lookup, and PyYAML workflow/invariant checks passed.
  `actionlint` was not available. An isolated MinGW 13.1 Release configure with
  `-Wa,-mbig-obj` discovered 82 top-level tests, built
  `sintra_barrier_processing_fence_backlog_test`, and the executable completed
  all 64 internal rounds with exit code `0` in 18.02 seconds.
- CI acceptance requires clean full-selection runs on Linux, FreeBSD, macOS, and
  Windows. This is the user-requested blocking recovery gate, but it remains a
  breadth gate and does not claim historical stress-depth restoration.
- Closure, 2026-07-10: the bounded remediation landed at `266fa2b` after an
  independent `gpt-5.5` xhigh implementation review returned push-ready green.
  Full-selection CI passed on the recovery branch for Linux (`29052340175`),
  FreeBSD (`29052340086`), macOS (`29052340069`), and Windows (`29052340133`),
  and on `master` for Linux (`29052340302`), FreeBSD (`29052340088`), macOS
  (`29052340584`), and Windows (`29052340173`). Coverage also passed with runner
  failure propagation enabled (`29052340089`). All platform runs discovered the
  complete 82-test roster. The restored 64-round processing-fence test passed
  in every exercised configuration, including macOS within the 90-second
  override.
- Separate architecture blocker/risk: the delivery-vs-processing contract and
  implementation relationship remains unsettled. The documented distinction is
  local-only proof for `delivery_fence_t` versus cross-participant processing
  proof for `processing_fence_t`; the misnamed backlog test is not a valid oracle
  for deciding which production side, if any, must change. Resolve that question
  in a separately reviewed architecture slice with a confirmed oracle. This
  remediation must not change production barrier, IPC, or runtime semantics.
- The remediation is closed. Slice 6 may proceed only to its existing scope
  validation; do not infer that the separate barrier-contract risk was resolved.

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

- Slice 4 dropped the minimal `330a29f` producer. If a future reopened Slice 4
  keeps it, do not duplicate it here.
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
- Do not treat the full-selection recovery gate as proof that historical stress
  depth is restored or that the branch is release-ready. It is the blocking
  breadth signal for this recovery branch.
- Do not patch around the same class of review finding twice. Stop and shrink
  the slice.

## Review Rule (Legacy Recovery Slices Only)

This rule governs only the pre-existing recovery slices below the custody
amendment. It cannot narrow, replace, inherit into, or override the managed-child
custody protocol above: every custody atom and batch boundary always requires
the maximum fresh independent `gpt-5.6-sol` xhigh priority reviewer pool and
unanimous unqualified GREEN.

This recovery plan gets the current six-Codex-plus-Claude review because the
baseline is uncertain. Future slices do not inherit that ceremony.

Before coding a slice, one independent reviewer checks only scope, named gate,
and whether the slice stayed small. Use a larger review only when the baseline
changes, the slice becomes multi-domain, or the same blocker class repeats.

## Next Action

Do not code in the preserved dirty worktree. The accepted and dropped slice
history above remains unchanged, Slice 5 is closed at `f95cf6e`, and the
test/CI recovery remediation is closed at `266fa2b`.

For the custody sibling, first evaluate the unique first-parent closure-commit
identity and fresh live protection predicate in **Finite Batch 0/1 Closure And
Batch 2 Authorization**. If both pass, Batch 0/1 remains closed even when HEAD
is a later first-parent descendant, and the next action is to freeze the current
Batch 2-or-later atom's exact files, causal gate, prohibited scope, verifier,
rollback, and reviewer lanes before delegation. If either fails, Batch 0/1 is
open or continuation is blocked: freeze this exact one-file closure-record
candidate, delegate `b01_closure_verifier_r3`, and run the fresh maximum boundary
pool `b01_boundary_arch_r4`, `b01_boundary_evidence_r4`, and
`b01_boundary_governance_r4` on the identical staged patch/raw-blob pair. Any
edit or non-GREEN restarts that full loop. Only then commit unchanged bytes with
the exact subject and trailers above. Do not infer closure from chat, an earlier
candidate round, a second-parent merge, or a partially matching commit.

The protected A0 evidence and its plan edits retain their separate ownership
and order. Slice 6 remains conditional after A0 evidence, its managed transport
successor, fence successor, and closure; open it only if duplicate group-
membership authority is then proven to block cleanup correctness, otherwise
durably drop it and move to Slice 7. Custody may land first with A0 still staged
and hash-protected. Before whichever overlapping track integrates second,
reconcile its plan edit with the already-landed canonical plan through the
separately delegated, frozen, verified, unanimous-review procedure above. The
delivery-vs-processing contract is not custody and must not be silently resolved
by this amendment.
