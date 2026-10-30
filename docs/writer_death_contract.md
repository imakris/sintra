# Writer-death delivery contract

The owner adopted the requirements below on 2026-10-05 and deferred their
implementation as a separate effort. No complete design has been adopted.
These requirements are not a claim that managed writer-death delivery is
already provided. Resume implementation only when the owner reopens the work
and the design receives the independent review required by [AGENTS.md](../AGENTS.md).

## Implementation boundary

Production ring readers retain a one-second blocking-wait watchdog.
`sintra_ring_semaphore` prepares its local handle and uses binary backend
posting, but timed waiting remains in `Ring_R::wait_for_new_data` in
`include/sintra/detail/ipc/rings.h`. The coordinator's crash handler still
calls `unpublish_transceiver_exact` from
`include/sintra/detail/process/managed_process_impl.h`. Wakeup improvements
therefore do not establish the complete native-death, drain, and unpublication
contract below, or authorize removing the watchdog.

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
- **Further owner decisions:**
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
  exists from reader preparation, whether or not the process ever publishes
  itself.
- **K6 Crash-safe notification.**
  - The primitive owns the binary token; no separate shared posting flags
    may suppress its wakeup. Quiescent reset always drains the token.
  - On POSIX every post issues the kernel wake, with no overflow early
    return.
  - A registration is popped only after its post completes.
  - On Windows the reader opens its slot's semaphore when it acquires the
    slot. A post that cannot reach its object is reported.
  - Every wake reason has a durable predicate checked under `m_spinlock` at
    registration.
  - Changes to shared ring layout require a ring ABI bump.
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
   not just end the reader thread.
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

## Unresolved design work

- **End-step execution.** Potentially blocking close, recovery, and reclaim
  work belongs on the existing owned lifecycle-worker facility. Callbacks for
  the last precondition record readiness and notify the owner. These steps
  cannot run on the coordinator's request reader or a shared exit dispatcher:
  takeover of a dead writer's lock can wait on a reused PID. The worker must
  observe coordinator teardown.
- **Retirement API.** Success means retirement intent for an exact occurrence
  was accepted or joined, not that retirement completed. Define the existing
  Boolean unpublish API accordingly. Reader threads cannot synchronously wait
  for completion, because local coordinator calls run inline. Answer a
  process's own unpublication request at admission, before it closes its
  writers. Completion must be separately observable.
- **Reclaim.** The terminal owner starts reclaim of the dead process's reader
  slots on the coordinator's rings on native death or observed departure,
  including death following an earlier intent. Register accounting before
  publishing the abandoned cursor.
- **Eviction composition.** Cancellation and `W` wait for completed relevant
  accounting, including reclaim and batches admitted during the drain. They
  never wait for future batches or a future death. Move the request write out
  of `keep_waiting_mutex` before composing the eviction protocol.
- **External attach.** Native observation must be ready before the external
  process's first ring commit, including its claim.
- **Interrupted raw-ring recovery.** Message-transport finality cannot depend
  solely on an owner word that recovery clears before a separate acquisition;
  a successor can die between those operations.
- **Raw-ring ownership.** Define close on behalf of a dead writer and the
  successor's flush when it takes writer ownership.
- **Empty streams.** Settle a child killed before constructing its writers,
  with untouched rings, head zero, and no recorded writer.
- **Wakeup proof.** A rescue detector must observe a POSIX token consumed after
  a kernel timeout. Predicate stores belong inside the posting lock so a live
  poster is not mistaken for a rescue. Revalidate each crash gap against the
  binary wakeup implementation rather than reintroducing removed flags.
- **Joining death.** Death joining a running terminal operation clears the
  dead writer's arbitration requests.
- **Pending calls and waiters.** A stopping reply reader flushes queued
  cancellations; `wait_for_instance` waiters for the ended occurrence are
  retired.
- **Teardown.** Coordinator teardown releases every held PID and handle.
- **Superseded paths.** Remove direct crash-handler unpublication and
  stop-and-discard retirement only when their replacement satisfies this
  contract. Reassess the pre-publication custody-release retry and the
  recovery-overlap limitation when the terminal transaction replaces them.
  Remove timed waiting and its timeout result only after the required proof.
  Reconcile the clarification in the design principles with the final contract.

If three causally distinct blockers appear at one seam after fixes, stop and
seek a bounded architecture review of that question. Review and validate each
implementation change before pushing, following [AGENTS.md](../AGENTS.md).

## Acceptance boundaries

Validation must cover Debug and Release with unrelated traffic unaffected:

- Kill a poster at every publication/wakeup gap with the watchdog disabled;
  use a parent supervisor deadline only to diagnose hangs, never for progress.
- Kill a managed child before publication and after it commits its own
  unpublication. Deliver every committed frame before unpublication, leave no
  stale name, and cover retirement followed by death.
- Prove occurrence binding: a late reply cannot reach a successor's reused call
  ID; a reply relayed before `W` wins over cancellation; a nonzero-occurrence
  caller of a coordinator-hosted object receives its reply.
- Drain a killed external process using native observation established before
  its first commit. Inventory raw-ring callers, including callers outside this
  repository, and prove they do not rely on timeout rescue.
- Run Windows, Linux, macOS, and FreeBSD with the watchdog disabled (a hang
  fails) and enabled (a rescue fails). An unbounded macOS wait must park without
  spinning. Only then remove timed waiting and confirm the full suite.

## Composition with the eviction and reader-death direction

The [adopted direction](design_principles.md#adopted-design-direction-under-review-not-implemented)
and this contract share their vocabulary: proven dead, reclaim with an account,
occurrence, `W`, "not delivered", "result lost".

- A leaf's death has two halves:
  - as a reader of the coordinator's rings, its slots are reclaimed with an
    account;
  - as a writer, its committed frames are delivered.

  Cancellations and the unpublication follow both halves.
- The coordinator's readers are never forced forward. So a leaf writer's
  committed tail is never abandoned and needs no account.
- This contract takes the eviction design's occurrence binding early and settles
  its unbound-call question: resolve first. The prefix change must be
  sequenced once, for both efforts.
- The eviction design's death order is generalized: both rings drain to the
  writer's end, and the escape "or has stopped" is removed. The crash
  notification is no longer a trigger.
- The eviction design's outcome-complete boundary also requires reply-ring
  gaps below `W` to be applied. That condition arrives with that direction.
- "Eviction and death are one mechanism" concerns an abandoned reader range.
  Writer death abandons none.

## Constraints and unresolved risks

- An unlisted lost-wakeup gap would hang without the watchdog; the
  supervised crash-gap and platform checks must expose it before removal.
- A coordinator-hosted handler that blocks indefinitely delays the drain and
  the unpublication (principle 3).
- On POSIX, a reused PID can delay takeover of a lock held by a dead external
  writer. Only managed children have their PID held.
- Crash events and recovery move later. A process that reports a crash and
  survives stays published but deaf until the returning-signal-handler
  contract is defined (see [deferred work](deferred_work.md#unresolved-investigation-questions)).
- A live process that never closes keeps its retirement and its name pending
  until it closes or is terminated.
- Raw-ring users that relied on the timeout's rescue lose it.
- Unbounded native macOS waits must park without spinning.
- Occurrence binding changes the wire format and can add a resolution round
  trip.
