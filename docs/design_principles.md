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
