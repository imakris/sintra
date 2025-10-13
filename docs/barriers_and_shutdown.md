# Barriers and shutdown semantics

Sintra distinguishes three lifecycle states for processes that participate in
coordinated execution:

- **ACTIVE** – the process contributes to barriers and other group-wide RPCs.
- **DRAINING** – the process is shutting down. It is excluded from future
  barriers and automatically released from any barrier that is already in
  progress.
- **TERMINATED** – the process has exited and its resources have been scavenged.

## `sintra::finalize()`

Calling `sintra::finalize()` now performs the following steps:

1. **Announce draining and quiesce.** The process issues
   `Coordinator::begin_process_draining` (or the RPC equivalent) while normal
   communication is still available. The coordinator records the new state,
   removes the caller from in-flight barriers, and returns the coordinator ring
   sequence that covers all messages accepted so far. Remote processes flush
   that sequence before proceeding, guaranteeing that all outstanding traffic is
   visible before teardown continues.【F:include/sintra/detail/sintra_impl.h†L148-L170】【F:include/sintra/detail/coordinator_impl.h†L55-L195】【F:include/sintra/detail/coordinator_impl.h†L455-L481】
2. **Unpublish under normal communication.** With the coordinator aware of the
   shutdown and the channel quiesced, the process deactivates handlers and
   unpublishes its transceivers before pausing the message readers. This keeps
   the synchronous unpublish path free of shutdown races.【F:include/sintra/detail/sintra_impl.h†L162-L170】
3. **Pause and destroy.** The process switches its readers to service mode,
   completes teardown, and releases all Sintra resources.【F:include/sintra/detail/sintra_impl.h†L162-L166】

## Barrier behaviour during shutdown

- Barrier membership is captured at the moment the coordinator starts the
  barrier. Any process that has already begun draining is skipped.【F:include/sintra/detail/coordinator_impl.h†L68-L111】
- If a process enters the draining state while a barrier is in progress, the
  coordinator removes it from the pending set and immediately completes the
  barrier if no other participants remain.【F:include/sintra/detail/coordinator_impl.h†L124-L200】
- Processes that already reached the barrier continue to wait for their return
  value. When the barrier resolves—either because the last active participant
  arrives or because every remaining pending member began draining—the
  coordinator sends the result to each waiting caller.【F:include/sintra/detail/coordinator_impl.h†L124-L200】

These rules eliminate shutdown deadlocks and let client code call
`sintra::finalize()` without inserting additional coordination barriers. If an
algorithm needs stronger guarantees about group membership, it should layer an
explicit membership protocol (or a future library helper) on top of these
barrier semantics.
