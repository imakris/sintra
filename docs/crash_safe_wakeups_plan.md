# Crash-safe ring wakeups

## Status and objective

Reopened by the owner on 2026-10-09. This is the active wakeup workstream of
[the writer-death and watchdog plan](writer_death_and_wait_watchdog_plan.md).
The first-slice direction was reviewed by Astra xhigh, Sol 6.1 xhigh and
Claude Sonnet 5.5 xhigh. The first slice is on the separate
`work/crash-safe-wakeups-20261009` branch: source commit `534d9dc3`, workflow
setup `75fb974a`, draft PR 862; no master publication. The corrected candidate
passed 84 focused runtime executions, 12 isolated Windows regressions and all
four seven-target builds. Source and all 28 selected binaries remained unchanged
across the renewed runtime gates. Astra/Sol follow-ups provide continuity
evidence. The original eligible gpt-5.6-sol xhigh evidence-package blocker remains
preserved historically; its matching-evidence follow-up cleared it and supplied
the actual clean independent pre-push review. All five focused hosted jobs
succeeded at `75fb974a`; MinGW supplied compilation coverage only.
The owner-requested full-CI run at published `75fb974a` completed successfully:
all six ordinary workflows and eleven jobs passed. This proves that published
first slice; it does not accept the staged N1 native-recovery continuation.
The full native-death objective and production 50 ms watchdog remain open.
The [native notification plan](native_exit_notification_plan.md) is adopted
for N1–N5 implementation after clean independent Astra xhigh and two Sol 6.1
xhigh reviews. The owner authorized the second Sol review in place of
quota-unavailable Claude. Its earlier denied launch remains historical evidence;
the later actual review completed cleanly. Native implementation acceptance,
N1–N5 completion and merge remain open.
The exact B9 combined N1 repair was adopted after three fresh independent
reviews found no design blockers. The r13 implementation review identified
reachable constructor, resource-custody and mandatory-error defects. The staged
candidate corrects those defects within the adopted repair and adds the missing
causal fixtures. Final matching-source platform gates and eligible independent
implementation acceptance remain open. Historical review and runtime failures
are preserved separately from subsequent repair evidence. The authoritative
design decision is
`sintra_crash_safe_wakeups_20261009/n1_mutex_boundary_review_20261010/root_adoption_r9_b9.md`.
Earlier provisional passes and
deliberately failing intermediate candidates are historical evidence, not
acceptance of this candidate. The full wakeup objective and its native-death
continuation remain open.

The objective is that a poster dying at any instruction cannot permanently
strand a surviving reader or prevent a later stop. Ultimately, a parked
reader must make progress from a real notification or native-death recovery,
without the 50 ms timeout. Making an interrupted post replayable is the first
implementation slice; it is not completion of this objective.

## Owner requirements

- Preserve the rings, star topology, one extraction into dispatch storage,
  fault isolation and trusted-coordinator boundary in
  [design principles](design_principles.md).
- Death is established by native evidence for the exact process incarnation;
  neither time nor a crash notification proves death. A raw ring's owner
  supplies its writer-death trigger.
- Preserve the accepted writer-tail delivery and occurrence-owned finality
  contracts if lifecycle code is touched. Wake recovery must not implement a
  stop-and-discard shortcut or close a successor's transport.
- Work on a separate branch and short-path worktree. Review the plan and
  major implementation changes with Astra xhigh, Sol 6.1 xhigh and Claude
  Sonnet 5.5 xhigh; report Claude unavailability if its credits prevent review.
- Validate fixes with focused local gates, then run full CI when the changes
  are ready, as the owner requires. Keep pushes paced and handle failures.

## Verified baseline

These citations were checked at `34017b49`. Paths below are under
`include/sintra/detail/` unless stated otherwise.

| Fact | Source |
|---|---|
| The shared `posted` flag is set before the backend post and suppresses later posts, including stop. `unordered` is a second shared reason flag. | `ipc/rings.h:1520`, `:1531` |
| A sleeping registration is removed before its post. | `ipc/rings.h:2220`, `:2511` |
| POSIX publishes its counter before calling the kernel wake; overflow returns without waking. | `ipc/semaphore.h:818` |
| Windows first opens the named object on a post or wait and caches that process's handle. The reader slot acquisition does not explicitly pin its object. The comment claiming independence from handle lifetime is incorrect. | `ipc/semaphore.h:52`, `:299`, `:445`; `ipc/rings.h:2838` |
| Reader registration, posting and cleanup share `m_spinlock`. The blocking wait has a 50 ms timeout. Close is checked before registration, whereas registration rechecks data, stop and global unblock. | `ipc/rings.h:2530`, `:3052`, `:3144`, `:3195`, `:3249` |
| Commit publishes its head under the posting lock; close and global-unblock predicates currently publish before acquiring that lock. Stop stores its predicate before `unblock_local()` locks. | `ipc/rings.h:3802`, `:3723`, `:3820`, `:3515` |
| A replacement writer resumes the committed head and clears arbitration requests, but does not replay sleeping registrations on acquisition. | `ipc/rings.h:3683` |
| Managed children already have occurrence-specific native-exit publication and subscriptions. Dispatch releases the lifeline, then queues subscribers; it does not wake these readers. | `process/managed_process_impl.h:4112`, `:6355` |
| Reader preparation precedes child creation. Reader objects have shared ownership and an occurrence. | `process/managed_process_impl.h:6112`, `:7005` |
| External writers exist before the attach claim. External reader retirement stops immediately. Native child observation alone therefore does not cover all writers. | `process/managed_process_impl.h:3493`, `:3639`, `:7047` |
| Crash notification still directly unpublishes. That path can stop readers and discard unread frames. | `process/managed_process_impl.h:3588`; `process/coordinator_impl.h:1606` |
| Active-target filtering already exists, separately from runtime selection. | `CMakeLists.txt:147`; `tests/CMakeLists.txt:59`; `TESTING.md:32` |

## Candidate first slice: replay-safe notification

This slice fixes ring notification itself while production retains the
watchdog. It does not alter process retirement, RPC cancellation or wire
occurrence binding.

1. Use one binary token in the primitive for ring wakeups. Remove the shared
   `posted` and `unordered` flags and reason-dependent suppression. Repeated
   ring posts coalesce without overflow, and every POSIX ring post performs
   the kernel wake even when a token is already present. Keep the generic
   counting-semaphore contract distinct from binary coalescing.
2. Pin the Windows object when acquiring a lifetime reader slot, before that
   slot becomes available to posters. Acquisition failure restores the slot
   and reports failure. A backend failure must not appear to be a successful
   post; a failed post leaves its registration recoverable.
3. Remove a sleeping registration only after posting succeeds. A crash
   between successful post and removal may cause a duplicate post; that must
   be harmless. Quiescent reset always drains the token while excluding
   posters and live waiters. Preserve cleanup and lifetime slot reuse.
4. Keep data, close, global-unblock and stop predicates authoritative, and
   check them under the registration lock before parking. Make predicate
   publication and its flush one locked operation where necessary. Preserve
   `unblock_local()`'s existing scope: it wakes a currently waiting reader.
5. A successor that actually acquires writer ownership replays outstanding
   registrations under the posting lock, using the existing committed head.
   This does not invent a writer or make takeover itself proof of a completed
   message-transport drain. This is a raw-ring path: managed replacements use
   new occurrence-specific ring files and cannot replay their predecessor's
   registrations by constructing a replacement writer.
6. Change the ring ABI fingerprint for the common-core semaphore layout and
   protocol change. Every process sharing that control block must agree; this
   is not an optional capability or a lifecycle-anchor format change.

Use the existing semaphore backends and lock ownership. Do not introduce a
second notification queue, an executor, a timer or a polling fallback.
Ordinary implementation details, including checked backend error plumbing,
belong to implementation and its review.

## First-slice implementation checkpoint

The source candidate uses a checked binary backend post and removes the two
shared reason flags. A flush attempts every outstanding registration once;
if any attempt fails, it retains the whole registration stack for harmless
replay. The publication path returns its captured committed sequence and
releases local writer ownership before reporting errors directly to stderr.
It cannot turn a notification failure into async-request rollback or a second
reply through a messaging caller's exception handler. Stop, close, global
unblock and writer-acquisition replay use the same checked posting boundary.
Generic counting-semaphore behavior is unchanged.

Reader admission checks quiescent reset and pins the Windows object before
publishing an active lifetime slot. Failed same-reader cleanup retains a local
reset obligation; re-registration must discharge it before publishing another
sleeping registration. The staged fixture now leaves and observes an actual
token at that failed-cleanup seam, then checks reset before that same reader's
next registration. Its explicit raw-owner replay rejects returned backend
errors. The normal roster adds only this fixture; the ABI fixture expects
layout revision 6 and rejects revision 5.

The frozen containment checkpoint and earlier raw evidence remain under
`C:\plms\varinomics\_agent_reports\sintra_crash_safe_wakeups_20261009\`.
The checkpoint records causal failures on the instrumented baseline,
provisional crash-gap passes, and request/reply failures from an intentionally
throwing intermediate post boundary. None was a gate of the final contained
candidate.

The resumed Windows Debug and Release builds of all seven focused targets
reported `FBuild: OK:`. Their first runtime attempt exposed a close-fixture
oracle error: `close_post_failure` returned an empty range with no pending
registration, but the fixture required the stopping flag on that first wake.
An isolated diagnostic reproduced those exact terms in both configurations.
The fixture now permits its established next-call close observation, as its
other close scenario already did, while separately requiring registration
cleanup. It passed three isolated runs per configuration after the correction;
production bytes were unchanged. The corrected complete Windows focused
schedule then passed 17 executions per configuration, including three complete
crash-fixture runs and ten counting-semaphore runs. Each of the four selected
IPC preservation cases also passed in both configurations: 42 requested
executions passed on Windows. The same schedule and selected IPC cases passed
on WSL Ubuntu 24.04 with GNU 13.3 in Debug and Release. All seven selected
targets built in both local platforms/configurations, and all 84 requested
runtime executions passed on the frozen production and corrected fixture
bytes of the pre-review candidate.

Current Astra and Sol 6.1 reviews identified two source blockers beyond that
runtime coverage: the Debug non-tail cleanup diagnostic invoked application
logging under the posting lock, and Windows cache/key allocations occurred
outside the checked handle helper's exception boundary. The new isolated
regressions reproduced the callback deadlock in Debug and allocation-path
failure in both Windows configurations. The corrected source preserves the
non-tail counter without calling application logging, and contains the whole
Windows handle lookup with checked `ENOMEM` and handle cleanup. Both new
regression selectors passed three isolated runs per Windows configuration
(12 executions total). The seven selected targets also built successfully in
Windows and WSL Debug and Release. The current fixed candidate then passed all
84 renewed focused runtime executions across those four configurations,
including the new platform-applicable regressions in each complete crash
fixture run. Source and all 28 selected executables matched before and after;
no rebuild was needed. Completed Astra/Sol continuity follow-ups found no
remaining source blockers. The original eligible Sol 5.6 evidence-package
blocker remains preserved as historical evidence; its matching-evidence
follow-up cleared that blocker and supplied the clean independent pre-push
review. The corrected local 84/84 and 12 isolated passes remain valid.
The first slice was committed on the work branch as `534d9dc3`; workflow setup
is `75fb974a`, with draft PR 862. Nothing has been pushed to master.
All five focused hosted jobs succeeded at `75fb974a`: Windows, Linux, macOS
and FreeBSD runtime lanes, plus MinGW compilation only. The owner-requested
full-CI run at that same published head completed successfully: all six ordinary
workflows and eleven jobs passed. Those results validate the published first
slice; they do not accept the staged N1 native-recovery implementation.
The production 50 ms watchdog and the native-death continuation remain pending
their own completion gates.

## Continuation needed for complete crash-safe wakes

The adopted continuation is [native-death notification and untimed
ring waits](native_exit_notification_plan.md). Its implementation and focused
completion gates remain open. Its final batch removes the production reader watchdog after
the native-owner gates; retaining the watchdog is not objective completion.

Replayability does not execute a replay: if the last poster dies and no
survivor touches the ring again, a parked reader still needs an owner to act.
Before removing the watchdog, connect exact native exit to notification for
every writer class and prove that registration after the exit cannot miss it.

The smallest candidate to investigate is **notification-only native-exit
replay**, keeping message-transport finality separate. Existing managed-child
exit subscriptions can notify the exact shared reader occurrence through the
existing lifecycle-worker facility. They must retain the relevant reader
lifetime, handle exit before subscription, and never act on a successor.
Native exit dispatch must not block on ring lock takeover.
Slice 1 therefore does not wake a parked coordinator reader merely because
its leaf writer died; that needs this continuation.

This candidate does not inherently require new RPC fields or a terminal
transaction: replaying a wake does not cancel a call, stop a reader or publish
transport completion. Nevertheless, current early stop/unpublication can
discard the tail independently of notification. The broader delivery promise
must remain explicitly pending until that path is changed under its accepted
contract; a wake test must not misreport it as solved.

External admission needs an exact native identity and an observer that cannot
miss a process that commits and exits during attach. Raw rings need a callable,
lifetime-safe owner recovery path and a consumer inventory. Interrupted
recovery must remain repeatable even if a recoverer dies; the mutex's transient
owner word alone cannot identify unfinished work. The coordinator's death
retains shared-fate semantics.

The design reviewers should decide whether notification-only recovery can
complete the wakeup objective without changing finality. If not, identify the
specific causal dependency and bring in only the corresponding part of the
writer-death plan. The entire eviction backlog is not a prerequisite by name.
Do not remove the timer or declare the full objective done while one of these
writer classes still depends on it.

## Focused gates and sequence

1. Review this scope and its completion boundary. Resolve concrete blockers;
   request a concrete fix and ask openly whether a simpler complete approach
   satisfies the contracts.
2. Add `ring_crash_safe_wakeup_test` with deterministic child-process seam
   tests using the existing test hooks
   and exact-child supervisor helpers. The parent's deadline diagnoses a hang
   and must never provide progress. A test-only switch disables the reader
   watchdog. Add compiled-out poster fault-injection hooks to the baseline
   before the causal production fix, then establish the affected failures.
   Prove that a waiter reached the native blocking wait; elapsed sleep alone
   is not evidence of a parked reader. Existing native lock-death evidence
   polling is separate from the reader watchdog and is not watchdog rescue.
3. Implement and independently review the first slice. Exercise death before
   posting, during token publication and kernel wake, and after posting but
   before removing the registration; then make the specified surviving actor
   retry. Cover the Windows post-before-first-wait/process-exit lifetime case,
   stop after an interrupted post, raw successor replay, close/registration
   races, and slot reuse with no stale token. Name and inject the close or
   global-unblock predicate-to-flush interruption explicitly. A late reader
   must observe the durable predicate without a post; a reader already parked
   at death requires a specified surviving replay actor. Actor-less parked
   reader recovery belongs to the native-death continuation. A successor that
   reopens a raw ring cannot stand in for replaying its predecessor's close.
4. Build only the new seam test and these selected preservation targets:
   `ring_wakeup_ownership_test`, `ring_wait_hint_test`,
   `interprocess_semaphore_test`, `process_reader_stop_ownership_test`, and
   `ring_abi_fingerprint_test`. Update the ABI fixture for the new layout and
   explicitly reject predecessor layout revision 5. Also cover the existing
   `ipc_rings_tests` writer-close/final-payload, replacement-writer/committed-
   head, new-data and local-unblock contracts: run those selected cases only
   or exercise equivalent preservation cases in `ring_crash_safe_wakeup_test`.
   Do not run the full IPC stress roster. Use bounded repetitions of meaningful
   interleavings in Debug and Release. Add a target only for a causal coverage
   need exposed by the implementation.
5. Run the same focused selection on Windows, Linux, macOS and FreeBSD CI,
   using the active-target filter and disabling unrelated examples/manual
   targets. Select before configure so CI neither builds nor runs the full
   roster. Preserve the normal roster and normal CI behavior outside this
   requested focused run; ensure pushing the branch cannot accidentally
   enqueue the ordinary broad build. Report focused evidence as focused.
6. After the notification recovery owner is implemented for each writer
   class, run its watchdog-disabled tests and production message-path
   preservation tests. Review that integration before deletion of the timer.
   Verify native macOS waiting actually parks, without spinning.

Local compiler-invoking commands use `queued-build` with explicit bounded
slots. Windows uses the required VS 2026 x64/v145 environment and XENIA
FASTBuild preflight, capture disabled and embedded debug information. The
worktree is `C:\w\scw1009`; build directories are separate and generator-pinned.

## Deletions, risks and next action

Delete the shared flags, reason-only branches and the Windows lifetime
misstatement in the first slice. Retain the production timeout until all
notification-owner gates pass; only then delete its result and retry remnants.
Do not delete the old lifecycle paths until their replacement owns the
accepted delivery/finality contract.

Remaining risks are an unlisted post interruption, stale slot/occurrence
notification, Windows acquisition or post failure, native-observer admission
gaps, and turning a POSIX counter update into apparent success only after a
kernel timeout. Tests with the watchdog disabled are the direct oracle for
this slice. Any later rescue detector must observe kernel timeout separately
from token consumption; ordinary idle timeout is not a rescue.

Next action: finish the N1 implementation and causal gates, then obtain its
independent implementation review and continue through N2–N5 in the
[adopted native notification plan](native_exit_notification_plan.md), including
production watchdog deletion. Design adoption does not establish implementation
acceptance or completion. The full native objective remains open.
