# Crash-safe ring wakeups

## Status and objective

Reopened by the owner on 2026-10-09. This is the active wakeup workstream of
[the writer-death and watchdog plan](writer_death_and_wait_watchdog_plan.md).
The first slice is adopted after independent Astra xhigh, Sol 6.1 xhigh and
Claude Sonnet 5.5 xhigh reviews found no remaining blockers. The full wakeup
objective and its native-death continuation remain open. No production
behavior has changed and no tests have run at this planning checkpoint.

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
- Build and run a focused local and CI set. The owner's current instruction
  supersedes the older plan's full-suite gates.

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

## Adopted first slice: replay-safe notification

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

## Continuation needed for complete crash-safe wakes

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

Next action: implement the reviewed first slice, starting with baseline
fault-injection failures, then run its focused gates. Continue toward the
full objective; report any
unavailable review or platform gate by its actual limitation.
