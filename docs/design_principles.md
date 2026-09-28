# Sintra design principles

Read this before proposing designs, reviewing, or changing ring, transport,
dispatch or lifecycle code. The owner set these rules. They override an
agent's own architectural preferences. The owner's words are quoted
verbatim, with typos corrected.

Code citations (`file:line`) refer to headers under `include/sintra/detail/`
at commit `a1c835e1`.

## Settled principles

**1. The rings and the star topology are the library.**
Every process writes its own request and reply rings. The coordinator reads
all of them and relays. Every other process reads only the coordinator's
two rings.
- Every process creates a reader for the coordinator (`managed_process_impl.h:3448-3456`).
- Only the coordinator creates readers for other processes (`managed_process_impl.h:5903`, `:5967`; `coordinator_impl.h:632`).

Never propose replacing the rings, or adding executors, inbox queues,
shared allocators or admission control, as a fix.
> "If we start discussing about replacing the rings, we are discussing about building a different library."

**2. Messages are delivered by value. Exactly one copy comes out of the ring.**
IPC must copy data out of shared memory, and by-value semantics are how that
copy happens. A second copy is a defect, not a trade-off.
> "Any double copy in this library is an indication of 'I couldn't do any better, for irrelevant reasons'."

On references into the ring, the owner "tends to lean more towards enforcing
better 'by value' semantics than allowing references into the ring".

**3. The coordinator is trusted infrastructure (the owner's kernel analogy).**
Sintra is a user-space library and the coordinator has no special privileges.
The analogy is about trust and shared fate, not privilege: the coordinator
plays the role a kernel plays in an operating system.

- The coordinator's own code, relay and the Coordinator service, is trusted.
  If it stalls, that is a Sintra bug to fix, not a condition to defend
  against.
- Application code hosted in the coordinator process shares the
  coordinator's fate, the way a driver loaded into a kernel shares the
  kernel's. If it misbehaves, the whole swarm suffers, and the fix belongs in
  the application.
- A stalled or paused coordinator stalls the swarm.
- Sintra does not route around any of this.

The owner's analogy:
> "If you put a crap driver to run with kernel privileges, well ok, then your system will suffer. If your kernel is crap, then your system will malfunction. There is no way around it."

**4. Sintra does not defend against its own bugs.**
A stall inside Sintra's own bounded code, such as copying or validating a
frame, is a bug to fix. It is not a condition to time out or work around.
> "The reading function belongs to sintra... I don't think we should try to protect from a failure of a sintra bug in one of the most important functions of its library."

Application code is different: handlers run on reader threads and may be
slow. That is a legitimate condition the design must handle.

**5. Fault isolation: only what was lost may fail.**
An eviction or a death must never fail an RPC, fence or barrier that had no
message in what was actually lost. Swarm-wide or other coarse failure
propagation is rejected.
> "We can't have irrelevant RPCs failing because some irrelevant slow reader was evicted. It is as if we have OS process failures happening because some irrelevant OS process did something wrong. No sane person would design that."

**6. A reader cannot hold a writer hostage by marking its slot.**
Readers are other processes and can die at any instruction.
> "There is no guarantee it's not going to crash right when it reads."

- Death is established from an operating-system process identity: a process
  handle, a pidfd, or PID plus start time.
- A timer is never proof of death.
- Signal and crash handlers are best effort and are never relied on:
  `SIGKILL` and `TerminateProcess` run no handler.

Deployment requirement (owner decision). From the capture of a reader
slot's start stamp until the slot is reclaimed, the processes of one swarm
must be able to observe one another through the native process-lookup
calls. Configurations that violate this are unsupported: liveness
classification may be wrong there.
- **Linux.** All processes of a swarm share one PID namespace and one time
  namespace, and neither changes during a slot's lifetime. The procfs at
  `/proc`, whose `/proc/self/stat`, `/proc/<pid>/stat` and
  `/proc/self/status` Sintra reads, is mounted for that PID namespace. No
  seccomp or similar filter fakes the results of `kill`, `pidfd_open` or
  procfs reads.
- **FreeBSD.** Processes of a swarm stay visible to one another through both
  `sysctl(KERN_PROC_PID)` and `kill(pid, 0)` with a positive PID. No MAC
  policy (Biba, MLS, `mac_seeotheruids`), `security.bsd.see_other_uids` or
  `see_other_gids`, `see_jail_proc`, jail boundary or credential change may
  hide one from another.
- **macOS.** No additional requirement.

FreeBSD start stamps (owner decision). FreeBSD offers no immutable
incarnation identity without held process references, which are out of
scope. Sintra's start stamp is `ki_start` minus `kern.boottime`, taken when
the boot time reads the same before and after the process record. A
wall-clock change moves both values and no snapshot covers the reads, so
that stability is best-effort evidence, never proof: a change reversed
between the two boot-time reads goes unseen. A matching stamp is LIVE. A
different stamp in a nonterminal record is UNKNOWN, never DEAD. A terminal
record, and a missing record with `ESRCH` from `kill(pid, 0)`, stay DEAD.
The residual behaviour:
- If a reader dies mid-copy and a new live process takes its PID before the
  check, its slot is reclaimed only once that process exits. Meanwhile a
  writer blocked by the slot fails the write with the UNKNOWN error instead
  of overwriting the copy.
- Mutex dead-owner recovery, lifecycle-attachment scavenging and run-directory
  cleanup wait the same way for the process holding the PID to exit.

Under this requirement, `ESRCH` from the native lookup means that the
process is absent. Death has two kinds of evidence, kept separate:
- **Absence** comes from the native PID lookup, which runs in the caller's
  own PID namespace. It never depends on procfs, namespace metadata or start
  stamps, and missing metadata never vetoes it.
- **A different incarnation or a terminal state** comes from a complete
  process record, and only where the record's coordinates are consistent
  with the published stamp. A known contradiction makes that evidence
  UNKNOWN, never DEAD. A lookup that finds some process while the published
  incarnation cannot be confirmed from a record is UNKNOWN.

**7. Define the contract before working around behaviour.**
When a mechanism causes trouble, first decide what it should do. Don't accept
its current behaviour and pay to work around it.
> "Maybe this way it solved the wrong problem... the right question should have been to define what eviction does exactly, in the first place, not take for granted that it does what it does and we have to work around it."

Prefer removing the conditions that create pressure over handling the pressure.

**8. Formal correctness, not "works on x86".**
Protection schemes that rely on data races are not used, even if common
practice tolerates them. That rules out seqlocks and other check-after-copy
schemes. Sintra supports ARM64: macOS on Apple Silicon.

**9. Messages after a gap are still delivered.**
A reader that lost an interval continues with the messages that follow.

## Adopted design direction (under review, not implemented)

This covers live-reader eviction and reader death. Design notes are
maintained outside the repository. The direction is not final and must not
be implemented until the design review concludes.
- Eviction becomes a **forced advance**. The writer moves the reader forward.
  Before doing so, it scans the headers of the messages the reader will skip
  and produces a precise outcome for each: "not delivered, not executed",
  "executed, result lost", or a fence failure for that receiver only.
- **Eviction and death are one mechanism.** When a reader's unread range is
  abandoned, the writer accounts for every message in it before reusing the
  space.
- **Only leaf readers are forced forward.** Readers in the coordinator process
  never are (principle 3).
- The reader protects its bounded copy with a **copying mark** in its own
  slot, not with a lock or a seqlock.

Current master still uses the interim protection:
- each frame is copied under the ring's shared `rs_stack_spinlock` (`rings.h:3420-3422`, `message.h:854`);
- slow readers are evicted after a 5 ms budget (`rings.h:162-172`);
- reader-slot owners are identified by PID alone (`rings.h:1845`).

## Working rules for agents

- **Keep three things visibly separate:** the owner's requirements, facts you
  verified in the code (cite `file:line`), and your own proposals. Never state
  a proposal as a requirement.
- **Stay in scope.** The task is to repair and improve Sintra, not to design a
  different IPC library. If a desired combination seems impossible within the
  principles, state the exact conflict and ask. Don't redefine the problem.
- **Keep review prompts neutral.** Prompts to other models or reviewers state
  the requirements and the evidence. They do not state a preferred mechanism,
  or highlight one candidate's weaknesses.
- **Adoption needs clean reviews.** A design is adopted only when independent
  reviewers report no blockers.
- **Builds and tests.** See [TESTING.md](../TESTING.md). On Windows, the test
  runner's scratch paths can exceed the 260-character path limit when the
  checkout is deeply nested, which causes false failures. Use a short
  checkout path.
- **Known CI flakes** (check these before suspecting a change):
  - `managed_child_custody_setup_race_contract_test`
  - `external_process_first_rpc_concurrency_test` on macOS Debug ("ping_failed: timed out waiting to release type resolution")

## Considered and rejected

These ideas were investigated and rejected. They are recorded so that they
are not proposed again without new evidence. The existing implementation is
the way it is for a reason. A proposal to change it must first show which
implicit guarantees it would have to rebuild.

**Replacing the processing barrier's second meeting with markers and acknowledgements** (2026-09-27, rejected).
Proposed: one meeting, then a coordinator marker that each participant's
readers acknowledge on reaching it. Gain: same latency, about a quarter fewer
ring messages, one round trip, not two. Two independent reviews found the
second meeting implicitly provides what markers must rebuild: ordering of
messages handlers send during the drain (the second arrival follows them on the
request ring); the coordinator's fence over traffic relayed mid-barrier; the
handler-caller exemption, tied to the calling thread; per-generation identity
(an old marker could satisfy a new one); last-arrival replies that cannot lose
a remap; departure handling; per-reader stop precision; unchanged wire format
for other modes. As specified, `post_handler_fence_order_test` would fail. The
gain did not justify a new protocol; no owner project uses processing barriers.
