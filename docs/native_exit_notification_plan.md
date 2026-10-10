# Native-death notification and untimed ring waits

## Status, objective and fixed contracts

This continuation of [crash-safe ring wakeups](crash_safe_wakeups_plan.md) was
adopted for N1–N5 implementation after independent Astra xhigh and two Sol 6.1
xhigh reviews reported no current design blockers. The owner authorized the
second independent Sol review in place of quota-unavailable Claude. N1 has a
staged implementation candidate; validation, independent implementation acceptance
and the full objective remain open.
The combined N1 publication, recursion and mandatory-cleanup repair (B9) was
adopted after three fresh independent reviews reported no design blockers.
The authoritative decision and preserved evidence are in
`sintra_crash_safe_wakeups_20261009/n1_mutex_boundary_review_20261010/root_adoption_r9_b9.md`.
The r13 implementation review identified reachable constructor, resource-custody
and mandatory-error defects. The staged source corrects those defects within the
adopted repair and adds the missing causal fixtures. Final matching-source
platform validation and eligible independent implementation acceptance remain
open; subsequent repair evidence does not replace historical failed evidence.
Historical baseline failures establish the defects and are not repaired passes.
The first slice makes
interrupted notification replayable; this continuation supplies the surviving
owner that performs that replay. Completion includes deletion of the production
50 ms parked-reader watchdog on Windows, Linux, macOS and FreeBSD.

Owner requirements are the [design principles](design_principles.md), the active
wakeup plan and the [writer-death contract](writer_death_and_wait_watchdog_plan.md):

- Keep the rings, star topology and one complete-frame extraction into stable
  dispatch storage. A surviving reader must make progress after a poster dies at
  any instruction, without a periodic reader wake.
- Native evidence for the exact process establishes death. Timers, crash
  notifications, retirement and loss of publication do not establish death.
- Notification recovery neither stops a reader, closes a ring, cancels an RPC,
  nor declares transport completion. It preserves the committed head and tail;
  only work affected by an actual lost range may fail.
- The coordinator and code in its process retain shared fate. Its death or
  internal stall is not routed around.
- Raw-ring owners supply the native-death trigger and its surviving recovery
  owner. Removing the watchdog must leave a usable, repeatable raw contract.
- Optional native observation owns independently versioned presence records or
  sidecars. Absence or an unknown capability version is a capability-scoped
  failure, not a common ring or lifecycle schema revision.

The proposal below separates notification completion from occurrence finality.
It does not claim to implement the pending cancellation, terminal-transaction or
precise eviction-accounting work. Existing stop-on-unpublication remains a known
committed-tail defect owned by the writer-death plan; the new observer must not
invoke or extend that path. If implementation exposes a causal dependency on
that path, bring the exact dependency into the affected batch rather than making
the entire finality backlog a prerequisite by name.

## Verified source boundary

These source facts were checked in the `work/crash-safe-wakeups-20261009` tree
at `75fb974a`. Paths are relative to `include/sintra/detail/`.

| Fact | Source |
|---|---|
| Managed reader preparation precedes native spawn; the reader retains occurrence and custody identity. | `process/managed_process_impl.h:6114`, `:7013` |
| The public observer selects the latest created occurrence and replays an already-recorded exit under custody protection. | `runtime.h:1927`, `:1953` |
| Native subscriptions run on one shared dispatcher; owned lifecycle workers have reserved ownership, admission and joining. | `process/managed_process_impl.h:4031`, `:3889`, `:4146` |
| POSIX ordinarily reaps before exit publication; Windows closes the original process handle after queued publication. | `process/managed_process_impl.h:3120`, `:2225` |
| Linux family mode has several consuming wait paths and closes its retained pidfds. | `process/native_process_family_impl.h:88`, `:138`, `:153` |
| Posting, registration and token cleanup use the posting lock; a failed flush retains registrations for replay. | `ipc/rings.h:2510`, `:2551`, `:3227` |
| `unblock_local()` affects a currently registered waiter; it has no persistent late-registration predicate. | `ipc/rings.h:3557` |
| Shared-spinlock recovery probes the process currently occupying the owner's PID, then CASes the process-instance owner word. | `ipc/spinlock.h:322`; `ipc/process_utils.h:424` |
| External readers are prepared at invitation, but external writers precede the ring-based claim; invitation arguments carry no native reference. | `process/coordinator_impl.h:624`; `process/managed_process_impl.h:3493`, `:3639`; `runtime.h:439`, `:471` |
| A non-stopping empty message-ring range retries; a continuously true death predicate would therefore spin. | `messaging/message.h:960` |
| POSIX `wait()` uses a huge finite timeout, and Darwin calls the timed address-wait API. | `ipc/semaphore.h:879`, `:727` |
| Unpublication still stops readers, independently of notification. | `process/coordinator_impl.h:1606` |

Existing `Native_family_external_child` is a maintained-direct-child authority
for Windows/Linux, not a four-platform external-attach observer. Incarnation
snapshot helpers also do not retain native observation or prove its readiness.

## Proposed notification ownership

Use one retained notification operation for each exact writer occurrence. It
owns native observation, the request/reply reader lifetimes, their ring mappings
and notification completion. For managed and external message rings this owner
lives in the coordinator and uses the existing owned lifecycle-worker facility.
Create its waiting worker before allowing the writer to run; allocation or
thread-start failure rejects setup truthfully instead of losing an exit callback
after it happens. Native callbacks only record readiness and notify that owner.
They never wait for a ring lock on the coordinator request reader or shared exit
dispatcher. No new executor or message inbox is introduced.

Bind this internal operation to the exact prepared occurrence, not to the public
"latest created" selector. Under the same occurrence protection, joining either
registers for its future native exit or observes its retained exit fact. An exit
before subscription, reader readiness or callback delivery therefore remains
actionable. Later causes join the same operation rather than replacing it.

Each reader has a retained, occurrence-bound native-exit notification edge and a
local consumed-edge state. The owner publishes the edge before attempting replay.
Registration checks it under the same protection as data, stop, close and global
unblock. An unconsumed edge prevents parking: return available committed data,
or one empty non-stopping notification when caught up. Once consumed, ordinary
parking is permitted again; death does not cause repeated empty returns.
Existing parked registrations are posted through the checked replay boundary.
The edge and pending replay survive a backend failure; success means the actual
required posts completed, not merely that the callback ran. A reader joining
after replay completion obtains the retained edge before it can park.

Replay completion covers the readers enrolled in that operation. New enrollment
after exit performs its own edge admission and checked replay; it cannot clear a
previous obligation. Keep the operation and mappings until enrolled reader
ownership is released and in-flight replay has joined. Unsubscription prevents
future callbacks and joins active delivery before destruction. Coordinator
teardown cancels local waiting work and joins it before closing native resources
or unmapping rings; cancellation of local work is not native-death evidence.

Checked backend failures remain visible and retryable. An injected transient
failure must be retried by the same retained operation before it reports
completion. Permanent native/backend failure reports an incomplete operation to
its owner; it is never called successful recovery and never restores the timer.

## Exact native lifetime and lock authority

Native identity, death evidence, numeric-PID reservation and authority to take
over a shared-lock owner are separate facts. The operation retains the exact
native witness and its bound process-instance word through replay. Add a narrow
proof-aware posting-lock acquisition: it may use the retained exact-death fact
only when the observed owner word matches that bound process instance and the
mapping belongs to the operation. Other owners still exclude; never cache "PID
is dead" globally or steal a live successor's ownership. The acquisition uses
the existing owner-word CAS and recovery bookkeeping. Wake replay does not
acquire writer ownership or clear the ownership mutex.

For managed POSIX children, observe exit with an exact non-consuming child wait
and retain the unreaped child until the notification owner releases its replay
hold. Preserve the sole-reaper ownership and exec-handshake exclusions. All
consuming paths, including Linux family mode and fallback cleanup, must honor
that hold. A held zombie must not starve other children: inspect the owned roster
and skip held results rather than repeatedly selecting one held `P_ALL` result.
Reap exactly once when the hold is released; native-empty/release reporting must
include the real retained native resource. On Windows retain a duplicate of the
original exact process handle for the operation; its ordinary observer can
retain its current publication semantics without closing the notification's
reference. An exit before any writer construction still settles notification
without inventing a committed frame or setting close.

For non-child writers, a Windows process handle or Linux non-thread pidfd remains
with the operation until replay and observation teardown complete. A pidfd is
not asserted to reserve its numeric PID. On macOS/FreeBSD retain the installed
process-exit filter and, once delivered, the occurrence-bound native exit fact;
do not attempt to reopen a numeric PID as if that were the old reference.

The existing process-instance token/stale-CAS ABA residual in the design
principles remains explicitly separate. This proposal preserves that documented
identity model; retained proof must not widen it into reusable authority for a
successor or unrelated mapping. Managed replay's unreaped-child hold removes
numeric-PID reuse during that replay. External/raw replay must retain its exact
bound owner word and reject a changed writer occurrence. Review must assess the
remaining documented token-collision residual explicitly; no claim that native
observation alone eliminates it is permitted. If the required contract demands
eliminating that residual, the concrete additional seam is the atomic lock-owner
identity representation, not RPC finality or the eviction backlog.

Native API evidence: Windows handles remain references after termination
([Microsoft](https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers)).
Linux pidfds report process exit through polling and have separate child-wait
semantics ([Linux manual](https://man7.org/linux/man-pages/man2/pidfd_open.2.html)).
Non-consuming child waits exist on FreeBSD
([wait manual](https://man.freebsd.org/cgi/man.cgi?query=wait&sektion=2&format=html))
and macOS ([Apple wait implementation](https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/kern/kern_exit.c)).

## External startup before the first writer

Add an invitation-owned, one-use native-observation handshake before external
writer construction. Use a named pipe on Windows and a local Unix-domain stream
socket on POSIX; these carry only bounded startup identity/readiness data. Ring
messages and the star topology remain the data transport. The optional handshake
has its own version and presence marker, tied to the invitation token and exact
occurrence, without changing the shared lifecycle-anchor schema.

The peer presents its own PID, process-instance word and occurrence over that
connected handshake. The coordinator reserves notification ownership and arms
the native observer, then reports observer readiness. The same peer confirms
readiness while still alive. Only after that confirmation does the coordinator
issue final writer permission; only after receiving it may the peer construct
writers or make its claim RPC. This live-peer confirmation binds a macOS/FreeBSD
numeric-PID process filter to the original process: that process was alive after
the filter was installed. Snapshots alone cannot supply this proof. Windows uses
an exact retained handle; Linux uses a non-thread pidfd. macOS/FreeBSD use
`EVFILT_PROC` with `NOTE_EXIT`; do not request unnecessary exit-status privileges.

An early exit before permission leaves no ring commit and settles setup without
native notification work for nonexistent writers. Once permission is issued,
invitation expiry/cancellation cannot discard the observer or prepared reader:
the original process may already be constructing writers. An observer event
arriving between confirmation, permission and writer construction is retained
and replayed for that exact occurrence. A failed/malformed/unknown handshake
rejects this capability before writer creation; it does not silently use the
old prewriter observation gap. Teardown joins handshake/observation work before
closing endpoints. Startup deadlines diagnose or cancel pre-permission setup;
they never establish death or provide parked-reader progress.

Process filters are attached to a process object and macOS captures subsequent
exit edges ([Apple source](https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/kern/kern_event.c));
FreeBSD documents process filtering and absent-process attachment errors
([kqueue manual](https://man.freebsd.org/cgi/man.cgi?query=kqueue&sektion=2&format=html)).
The live confirmation closes the before-registration edge gap; kernel events,
not socket closure or elapsed time, establish native death.

## Raw-ring owner contract and known callers

Expose one checked notification-replay operation on an owned raw-ring recovery
binding. The binding retains the exact ring generation/mapping, enrolled reader
lifetimes and writer native authority. The raw owner arms the native trigger
before allowing a cross-process writer to commit. It invokes or delegates replay
on exact exit, and may repeat the same operation after interruption or checked
failure. Reopening a successor writer is not predecessor-close recovery.

Publish the occurrence's death edge and unfinished notification obligation
before touching the posting lock. These outlive one recoverer. For recovery
delegated to another process, the raw owner also retains that recoverer's exact
native authority and arranges a surviving actor to resume if it dies while
holding the posting lock or after posting but before completion. No transient
mutex owner word is an obligation ledger. Raw owners using multiple processes
that can hold the posting lock must cover those process instances in this
authority/lifetime domain. Same-process-only rings need no foreign-death trigger.
This is the owner contract, not an automatic OS watcher hidden in every raw
reader, and it introduces no new generic execution or admission mechanism.

The known maintained roster is Phylax, Lumis and vnm_framework. The retained
inventory and current static checks identify Phylax processor logs, processor
inputs and frame descriptors; Lumis strategy logs, orders and inputs; framework
LOD reader access. The log/order consumers retain shared reader lifetimes and
hosted-child custody; framework already wraps the custody exit observer with
weak lifetime state and a retained subscription. Compose the new raw binding at
that owner boundary. Input/descriptor producers hosted in the coordinator keep
shared fate; classify actual producer ownership before adding a foreign-death
subscription. Snapshot-only plot consumers do not park and need no wake trigger.
The Sintra contract and tests can progress without inventing an unknown consumer
roster or making external integration a prerequisite for Sintra CI.

## Implementation sequence and focused gates

| Batch and affected surface | Dependency and acceptance |
|---|---|
| N1: ring notification binding/replay and proof-aware posting-lock acquisition; `ipc/rings.h`, `ipc/spinlock.h`, focused wake tests and documentation | Start after independent design adoption. With watchdog disabled, kill the writer at the existing publication/post seams and let only its armed native owner act. Cover late enrollment/registration, a checked transient post failure, changed owner/occurrence rejection, intact committed data, and stop after recovery. |
| N2: exact managed observer, replay lifetime and child reap/handle holds; managed process/readers/native-family code and lifecycle seam tests | N1 supplies the checked replay boundary. Kill before writer construction, before/after exit subscription, while each ring is parked, and after committed data. Exercise all reaping modes, two concurrent exits with one held result, worker allocation failure before spawn, replacement and teardown. No request-reader or shared-dispatcher blocking; unrelated traffic remains deliverable. |
| N3: invitation-owned external observation handshake; runtime/coordinator/managed-process code and external startup tests | N1/N2 ownership is available. Kill at each readiness/permission/first-commit boundary on every supported OS. Verify no pre-permission commit, retained death between permission and claim, cancellation races, malformed/absent capability rejection, endpoint/observer teardown and unrelated invitations. |
| N4: public raw recovery binding and owner contract; ring API/docs and raw seam tests | N1 boundary plus exact native trigger ownership. Kill a recoverer before/after death-edge publication, during each post and before completion; a specified surviving owner resumes. Test predecessor/successor isolation and late reader enrollment. Known consumer composition is a named integration follow-up on those owner files; it is not an unknown-roster gate. |
| N5: actual untimed native waits and watchdog deletion; `ipc/semaphore.h`, `ipc/rings.h`, focused tests/docs | N1-N4 native-owner gates pass on Windows, Linux, macOS and FreeBSD. Remove the production 50 ms wait, timeout branch and rescue-only test switch. Run the production path untimed; macOS must park in the native untimed API rather than loop on an oversized timeout error. |

Use deterministic child-process seams and native wait-entry evidence; supervisor
deadlines diagnose hangs and never provide progress. Preserve the first-slice
focused selection: crash-safe wakeups, wakeup ownership, wait hints, semaphore,
reader stop ownership and ring ABI, with the selected close/final-payload and
replacement/head cases. Add managed/external/raw targets only for the causal
boundaries above. Each batch runs Debug and Release locally where available,
then the same focused four-platform CI. Required independent implementation
review precedes its publication. The owner's requested ordinary full-CI run is
handled separately by root; focused evidence is not full-suite certification.

N5 gives POSIX `wait()` a true untimed backend: futex with no timeout on Linux,
untimed UMTX on FreeBSD, and `os_sync_wait_on_address` with the shared-memory flag
on macOS. Windows keeps its infinite native wait. Token acquisition/recheck and
checked native errors stay authoritative; timed generic semaphore APIs remain
available for callers that explicitly request deadlines. Apple documents the
untimed atomic compare-and-wait and matching shared wake flags
([wait API](https://developer.apple.com/documentation/os/os_sync_wait_on_address?changes=_1),
[shared flag](https://developer.apple.com/documentation/os/os_sync_wait_on_address_flags_t/os_sync_wait_on_address_shared)).
Prove actual blocking and bounded native wait-entry counts, including ARM64.

## Deletions, risks and next action

N5 deletes the reader watchdog constant, timeout-only state/retry branches and
its test-only disable switch, and replaces the huge-timeout implementation of
untimed POSIX waits. Keep generic timed semaphores and lock liveness observation;
those are separate contracts. Replace stale documentation that attributes
progress to reader polling. Delete any superseded notification path in its
owning batch. Do not delete old finality paths before their contract is replaced.

Material risks are a missed native/registration edge, raw owner lifetime gaps,
native reference released before replay, early external permission/cancellation,
PID/token ABA, permanently failed kernel notification, and native wait errors
that turn parking into spinning. The named gates address these boundaries;
unsupported observation or unavailable platform/reviewer is reported truthfully.

Next action: implement and causally validate N1, including proof before reader
construction and through cleanup, retained non-consuming native fixture authority,
and dead-writer REQUEST cleanup serialized against successor acquisition.
The independent design gate is complete; implementation review remains required.
Continue through N5 and delete the production watchdog after the native-owner
platform gates pass. N1 completion does not establish the full objective.
