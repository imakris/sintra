# Writer death and the parked-reader watchdog: refactoring plan

## Status

**Wakeup work reopened on 2026-10-09.** The owner requested implementation of
crash-safe wakeups with focused local and CI tests, in a separate branch and
worktree. The active scope and notification/finality boundary are in the
[crash-safe wakeups plan](crash_safe_wakeups_plan.md). Its first notification
slice has passed the requested independent reviews. The native-death
continuation remains open. The broader writer-death delivery
refactor remains deferred except for dependencies established by that review.
The contracts decided on 2026-10-05 still apply when lifecycle code is touched.
The historical stages below provide context; their full-suite gates are
superseded by the owner's focused-test instruction for this effort.

Source citations (`file:line`) refer to headers under `include/sintra/detail/`
at `c03e0d27`. Short names: `rings.h`, `semaphore.h`, `mutex.h`,
`spinlock.h`, `process_utils.h` are under `ipc/`; `pmr_impl.h` is
`messaging/process_message_reader_impl.h`; `mp_impl.h` and `coord_impl.h` are
`process/managed_process_impl.h` and `process/coordinator_impl.h`.

## Objective

1. When a writer process other than the coordinator dies, every message it
   committed is delivered.
2. Reader wakeups survive a poster dying at any instruction, so that the
   50 ms parked-reader timeout can be removed without any time-based
   correctness.

## Current behaviour

These facts were verified by reading the source at `c03e0d27`; nothing was
built or run.

- **The watchdog.** A caught-up reader registers on the ring's sleeping stack
  under `m_spinlock` and waits on its slot's semaphore with a 50 ms timeout
  (`rings.h:3052`, `3195-3257`). On timeout it cleans up and parks again.
- **Between live processes the handshake is lossless.** Data, global unblock,
  writer close and stop are each posted under `m_spinlock`, and the reader
  rechecks every predicate when it registers (`rings.h:3144-3149`,
  `3186-3218`, `3506-3519`, `3802-3824`).
- **The timeout covers crash and lifetime gaps only:**

  | Gap | Where |
  |---|---|
  | Windows: the writer posts, then its process exits before the reader opens its first handle; the named semaphore and its token disappear. No crash is needed. | `semaphore.h:293-353`, `523-544`; the comment at `semaphore.h:52` claiming the object outlives its handles is wrong |
  | The writer dies between setting the shared `posted` flag and the backend post; later posts to that slot, including the reader's own stop, are swallowed | `rings.h:1520-1537` |
  | POSIX: the writer dies between the counter update and the kernel wake | `semaphore.h:820-842` |
  | The writer dies after publishing and before or during the flush; the drained entry was already popped | `rings.h:3807-3808`, `2220-2229` |
  | The writer dies after `writer_closed = 1` or the unblock increment, before the flush | `rings.h:3723-3724`, `3820-3823` |

  Without the timeout the first three also leave the reader unstoppable, and
  `~Process_message_reader` ends in `exit(1)` (`pmr_impl.h:771-778`).
- **Stop is not drain.** A stopping reader discards unread committed frames
  (`rings.h:3597-3599`, `messaging/message.h:1002-1004`).
- **Crash notification.** The coordinator's handler for
  `terminated_abnormally` unpublishes the process and stops its readers
  (`mp_impl.h:3588-3612`, `coord_impl.h:1576`, `1606-1613`), discarding any
  committed frames still unread.
- **Unannounced native exit** (a kill, or a crash whose notification never
  left). The exit is recorded and the lifeline released
  (`mp_impl.h:2176-2228`, `3083-3145`, `4112-4131`), but nothing unpublishes
  the process or stops its readers.

Whether a dead writer's last committed messages are delivered therefore
depends on timing: on whether a 50 ms wakeup happens before a stop.

## Owner decisions (2026-10-05)

- **Delivery.** A crashed writer's committed messages are delivered.
- **Boundary: occurrence-owned finality.** A leaf transport (the
  coordinator's two readers of one process occurrence) ends only at its
  writer's own close, at the exact native death of that occurrence, or at a
  definitive cancellation before any writer can start. A process's own
  unpublication, and any retirement, only record intent.
- **Retiring a live process** waits for its writer's own end, so everything
  it commits is delivered. A process that never closes stays pending until it
  closes or is terminated; termination is the death path.
- **Further decisions**, as recommended by the design (numbered as in its
  notes):
  - OD-1: the coordinator as a writer is outside the guarantee. Its death is
    swarm death (principle 3).
  - OD-2: an external process is admitted only with a native exit handle for
    its incarnation.
  - OD-3: `terminated_abnormally` records crash provenance only.
    Unpublication, the lifecycle event and recovery follow native exit and
    the drain.
  - OD-4: an unannounced native exit ends the occurrence: unpublication,
    lifecycle event and recovery consideration.
  - OD-5: retirement drains; it never discards committed frames.
  - OD-7: for raw rings, the ring's owner supplies the death trigger.
  - OD-8: every call is bound to a target occurrence. When the caller does
    not know the occurrence, it resolves it before calling, so no call is
    unbound.
- **"Sender gone" indication (OD-6): adapted.** Replies to a caller known to
  be dead are dropped through occurrence binding. Handlers get no flag: what the
  coordinator knows at delivery time is a timing-dependent lower bound, and
  branching on it would make the effect of a dead process's final requests
  depend on timing.

## Settled contract kernel

Reviewers of both design rounds left these unchallenged, or confirmed them.

- **K1 Native evidence only.** Death is established only from native exit of
  the exact incarnation (principle 6). `terminated_abnormally` is provenance.
- **K2 Death delivers.** On proven death, every committed frame of the dead
  occurrence is delivered with the ordinary relay and dispatch semantics.
- **K3 Order.** The coordinator cancels its own remaining calls to the
  occurrence after the **reply** drain, not after the request drain. A
  coordinator-hosted handler on the request reader that is blocked in a call
  to the dead process would otherwise deadlock. The unpublication follows
  both drains and every request relay, and carries `W`, the coordinator's
  reply-ring head, at or above every relayed reply. Other processes cancel
  calls bound to the occurrence only after their reply progress passes `W`.
- **K4 Occurrence binding.** Requests and replies carry occurrences, each
  with a presence flag (occurrence 0 is a valid original launch).
  - The coordinator establishes caller identity at ingress, before direct
    dispatch or relay. Replies the coordinator writes itself are bound too.
  - Each process checks against its canonical transport occurrence; for
    external processes that is the attach occurrence.
  - Live-occurrence tables merge monotonically.
  - A request bound to an ended occurrence is rejected as not dispatched. A
    reply bound to a caller occurrence that is not live is dropped.
- **K5 The transport exists before publication.** A transport occurrence
  exists from reader preparation (`mp_impl.h:6112`), whether or not the
  process ever publishes itself.
- **K6 Crash-safe notification.**
  - Remove the shared `posted` and `unordered` flags. The token is binary
    inside the primitive, and quiescent reset always drains it.
  - On POSIX every post issues the kernel wake, with no overflow early
    return.
  - A registration is popped only after its post completes.
  - On Windows the reader opens its slot's semaphore when it acquires the
    slot. A post that cannot reach its object is reported.
  - Every wake reason has a durable predicate checked under `m_spinlock` at
    registration.
  - The ring ABI is bumped.
- **K7 No early stop.** No reader is stopped or destroyed before its
  transport ends, except by the coordinator's own teardown (principle 3).
- **K8** The "sender gone" decision above.

## The chosen boundary

These invariants come from the architecture check, accepted by the owner.

1. **Intent only.** Self-unpublication and retirement record intent. Final
   heads are fixed only by the writer's close, exact native death, or
   cancellation before any writer can start. No observed unpublication
   shortens them.
2. **Independent drains.** Both readers deliver their complete committed
   streams. Reaching end of stream records that ring's completion; it does
   not just end the reader thread. Today a reader reaching `writer_closed`
   stops on its own and bypasses `stop_nowait` (`rings.h:3144-3149`,
   `pmr_impl.h:1213`).
3. **Cancellation after the reply drain.** Once the reply drain and the
   applicable eviction accounting are complete, the coordinator cancels its
   remaining calls bound to the occurrence. The request drain can then
   finish.
4. **One terminal transaction.** After both drains, one terminal request
   follows every request relay. The coordinator's own request reader then
   unpublishes the occurrence, or settles a startup that was never
   published.
5. **One owner.** A terminal owner starts eligible reclaim, owns completion
   and release, and keeps the readers and the native identity until
   release. For a managed child it holds the dead PID: POSIX observes the
   exit without reaping it until release, and Windows keeps the process
   handle open. Later causes join the running operation; the coordinator's
   teardown is the shared-fate exception.

## What design revision 3 must settle

- **End-step executor.** Potentially blocking close, recovery and reclaim
  work runs on the existing owned lifecycle-worker facility
  (`mp_impl.h:3889`; used for external reader retirement at
  `mp_impl.h:7067`). Callbacks for the last precondition only record
  readiness and notify the owner. These steps never run on the coordinator's
  request reader or a shared exit dispatcher: taking over a dead writer's
  spinlock can wait on a reused PID (`process_utils.h:424-435`,
  `spinlock.h:216-243`). The worker observes coordinator teardown.
- **Retirement API semantics.** Success means "retirement intent for this
  exact occurrence was accepted or joined", not completion.
  - Define the meaning of the existing Boolean unpublish API.
  - Reader threads cannot wait synchronously for completion: local
    coordinator calls run inline (`transceiver_impl.h:1523`).
  - A process's own unpublication request is answered at admission, before
    the process closes its writers.
  - Completion is separately observable.
- **Reclaim.** The terminal owner starts reclaim of the dead process's reader
  slots on the coordinator's rings, on native death or observed departure,
  including a death that arrives after an earlier intent. Accounting work is
  registered before the abandoned cursor is published.
- **Composition invariant with eviction.** Cancellation and `W` wait for the
  completed relevant accounting, including that reclaim and batches admitted
  during the drain. They never wait for future batches or a future death.
- **Prerequisite seams:**
  - **External attach.** Native observation must be ready before the
    external process's first ring commit, including its claim. Its writers
    exist before the claim RPC (`mp_impl.h:3493-3497`, `3639`), and its
    readers are prepared at invitation (`coord_impl.h:624`).
  - **Interrupted raw-ring recovery.** Dead-owner recovery clears the owner
    word (`mutex.h:403`) before a separate acquisition (`mutex.h:349`). A
    successor dying between the two erases the record of an unfinished
    close. Message-transport finality must not depend on that record alone.
- **Empty-stream settlement.** A child killed before it constructs its
  writers leaves untouched rings: head zero, no recorded writer
  (`rings.h:2276`).
- **Round-2 review items:**
  - The rescue detector must see a POSIX token consumed after a kernel
    timeout (`semaphore.h:795-806`). Predicate stores move inside the posting
    lock, so a live poster is never mistaken for a rescue.
  - A death that joins a running operation clears the dead writer's
    arbitration requests.
  - A stopping reply reader flushes queued cancellations.
  - `wait_for_instance` waiters of the ended occurrence are retired.
  - The eviction design's P-F prerequisite comes first: the request write
    must move out of `keep_waiting_mutex`.
  - Coordinator teardown still releases held PIDs and handles.
- **Clarification text.** Restate the dated clarification in
  [design principles](design_principles.md) to the final contract.
- **Deletions to confirm.** The shared `posted` and `unordered` flags; the
  crash handler's direct unpublish (`mp_impl.h:3609`); stop-and-discard on
  unpublication; the pre-publication "first miss" retry in custody release
  (`mp_impl.h:5297-5340`), if the terminal transaction replaces it; the
  recovery-overlap note in the lifecycle-hooks reference; and, in S4, the
  timed wait and its timeout result.

## Stages and gates

Land one reviewed change at a time, following the CI workflow in
[AGENTS.md](../AGENTS.md). Production keeps the 50 ms timeout until S4.
Every gate runs in Debug and Release, with unrelated traffic that must be
unaffected.

| Stage | Content | Gate |
|---|---|---|
| S0 | Test-only watchdog switch; rescue detector based on registration and token state; supervisor helpers | No production change; existing suite green |
| S1 | Seam tests that kill a writer at each gap, and contract tests | Expected failures fail on current code with the watchdog disabled; preservation tests pass |
| S2a | Crash-safe notification (K6), successor flush and ABI work now follow the [active wakeup plan](crash_safe_wakeups_plan.md); raw-ring close on behalf remains a separate finality dependency | Focused gates and explicit retry actors in the active plan; full native-death recovery remains its continuation |
| S2b | Occurrence binding (K4); `W` in unpublication | A late reply cannot reach a successor's reused call id; a reply relayed before `W` wins over cancellation; a nonzero-occurrence caller of a coordinator-hosted object gets its reply |
| S2c | Transport ending for managed children (the chosen boundary); crash notification becomes provenance | Kill a child at every stage, including before it publishes and after it commits its own unpublication: every committed frame is delivered, the unpublication follows them, and no stale name remains; a retirement is followed by death; lifecycle and recovery suites updated |
| S2d | Native exit handles for external processes, ready before the first commit | A killed external process is drained like a child |
| S2e | Raw-ring contract and inventory of raw-ring users outside the repository | No user relies on the timeout's rescue |
| S3 | Four platforms, two lanes: watchdog off, where a hang is a supervisor failure; watchdog on, where any rescue is a failure | Both lanes green; the unbounded macOS wait parks without spinning |
| S4 | Remove the timed wait and its remnants | Full suite green on Windows, Linux, macOS and FreeBSD |

Each seam test runs the parked reader in a child process under a deadline set
by the parent. The deadline only diagnoses failures and is never part of
progress.

## Review protocol

- A design is adopted only when two independent read-only reviewers, both at
  their highest reasoning setting, report no blockers.
- Prompts state the requirements and evidence, not a preferred mechanism.
  They ask for a concrete fix per blocker, and openly whether a simpler
  approach exists.
- When three causally distinct blockers appear at one seam after fixes,
  stop. Write down the architecture question and get a bounded check instead
  of patching.
- Each implementation stage is reviewed and validated before it is pushed.

## Composition with the eviction and reader-death direction

The [adopted direction](design_principles.md#adopted-design-direction-under-review-not-implemented)
and this plan share their vocabulary: proven dead, reclaim with an account,
occurrence, `W`, "not delivered", "result lost".

- A leaf's death has two halves:
  - as a reader of the coordinator's rings, its slots are reclaimed with an
    account;
  - as a writer, its committed frames are delivered.

  Cancellations and the unpublication follow both halves.
- The coordinator's readers are never forced forward. So a leaf writer's
  committed tail is never abandoned and needs no account.
- This plan takes the eviction design's occurrence binding early and settles
  its unbound-call question: resolve first. The prefix change must be
  sequenced once, for both efforts.
- The eviction design's death order is generalized: both rings drain to the
  writer's end, and the escape "or has stopped" is removed. The crash
  notification is no longer a trigger.
- The eviction design's outcome-complete boundary also requires reply-ring
  gaps below `W` to be applied. That condition arrives with that direction.
- "Eviction and death are one mechanism" concerns an abandoned reader range.
  Writer death abandons none.

## Risks

- An unlisted lost-wakeup gap would hang after S4. S1 and S3 exist to expose
  one first.
- A coordinator-hosted handler that blocks indefinitely delays the drain and
  the unpublication (principle 3).
- On POSIX, a reused PID can delay takeover of a lock held by a dead external
  writer. Only managed children have their PID held.
- Crash events and recovery move later. A process that reports a crash and
  survives stays published but deaf until the returning-signal-handler
  contract is defined (see [deferred work](deferred_work.md#historical-observations-to-revalidate-if-reopened)).
- A live process that never closes keeps its retirement and its name pending
  until it closes or is terminated.
- Raw-ring users that relied on the timeout's rescue lose it.
- Unbounded native macOS waits are new.
- Occurrence binding changes the wire format and can add a resolution round
  trip.
- Nothing has been measured.

## Design history

Analysis and review rounds were kept outside this repository, under
`C:\plms\varinomics\_agent_reports\`:

- `sintra_writer_death_20261005/`: `DESIGN.r1.md`, `DESIGN.r2.md`, the
  contracts, `ARCHITECTURE_QUESTION.md`, `arch_check_codex_astra.md`, the
  review rounds (`review_r1_*`, `review_r2_*`) and `STATUS.md`.
- `soter_idle_parity_20261005/sintra_wait/`: `SYNTHESIS.md`,
  `codex_astra_xhigh.md`, `claude_analysis.md`. These are independent
  analyses of the 50 ms timeout, at `3a5c9ec5`.
- `sintra_eviction_20260927/`: the eviction and reader-death design,
  revision 7.

These files are design history, not API authority. This plan preserves the
decisions and the remaining scope without them.
