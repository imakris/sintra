# Barriers and shutdown semantics

Sintra distinguishes three lifecycle states for processes that participate in
coordinated execution:

- **ACTIVE** - the process contributes to barriers and other group-wide RPCs.
- **DRAINING** - the process is shutting down. It is excluded from future
  barriers and automatically released from any barrier that is already in
  progress.
- **TERMINATED** - the process has exited and its resources have been scavenged.

## `sintra::finalize()`

Calling `sintra::finalize()` now performs the following steps:

1. **Announce draining and quiesce.** The process issues
   `Coordinator::begin_process_draining` (or the RPC equivalent) while normal
   communication is still available. The coordinator records the new state,
   removes the caller from in-flight barriers, and returns a **reply-ring
   watermark** (`m_out_rep_c`) for the caller that covers all messages accepted
   so far. The coordinator separately computes per-recipient tokens when
   emitting completions; the return value is a single marker for the caller to
   flush. Remote processes flush that sequence before proceeding, guaranteeing
   that all outstanding traffic is visible before teardown continues, while the
   coordinator path additionally blocks in `wait_for_all_draining()` until
   every known process has entered the draining state (or been scavenged).
   *(Coordinator::begin_process_draining and
   Coordinator::wait_for_all_draining in coordinator_impl.h)*
2. **Pause, then unpublish under service-mode communication.** With the
   coordinator aware of the shutdown and remote callers flushed, the process
   first switches its readers to service mode via `pause()`. In this mode only
   coordinator/service messages are processed. While paused, the process
   deactivates handlers and unpublishes its transceivers (via
   `deactivate_all()` / `unpublish_all_transceivers()`), keeping the synchronous
   unpublish path free of shutdown races while still allowing coordinator RPCs
   to flow.
3. **Destroy the managed process.** After transceivers are unpublished, the
   runtime destroys the `Managed_process` singleton. Its destructor stops the
   reader threads and releases all Sintra resources owned by the process.

## Barrier behaviour during shutdown

- **Atomic barrier start with draining filter:** Barrier membership is captured
  atomically at the moment the coordinator starts the barrier. The membership
  snapshot and draining-state filtering occur under the same `m_call_mutex` lock,
  ensuring no process can be added/removed or change draining state during this
  critical window. Any process that has already begun draining is skipped.
  *(Process_group::barrier in coordinator_impl.h)*
- **Process removal during barrier:** If a process enters the draining state while
  a barrier is in progress, the coordinator removes it from the pending set via
  `drop_from_inflight_barriers()` and immediately completes the barrier if no other
  participants remain. *(Coordinator::drop_from_inflight_barriers in coordinator_impl.h)*
- **Barrier completion with per-recipient flush tokens:** Processes that already
  reached the barrier continue to wait for their return value. When the barrier
  resolves-either because the last active participant arrives or because every
  remaining pending member began draining-the coordinator sends the result to each
  waiting caller. **Critically, each recipient receives a flush token computed at
  the moment their message is written** (not a global token), preventing hangs
  where a global watermark might be ahead of a recipient's channel.
  *(Process_group::barrier in coordinator_impl.h)*

### Draining state lifecycle

- **Setting the draining bit:** The draining bit is set in two scenarios:
  1. When `begin_process_draining()` is explicitly called during graceful shutdown
  2. When `unpublish_transceiver()` is called for a `Managed_process` and no prior
     draining call was made (crash/ungraceful shutdown scenario)
- **Draining bit lifetime:** Once set, the draining bit is **never cleared during
  teardown**. It persists until a new process is published into the same slot,
  at which point it is reset to ACTIVE. This prevents races where resetting too
  early allows concurrent barriers to include a dying process.
  *(Coordinator::begin_process_draining and Coordinator::drop_from_inflight_barriers in coordinator_impl.h)*

These rules eliminate shutdown deadlocks and let client code call
`sintra::finalize()` without inserting additional coordination barriers. If an
algorithm needs stronger guarantees about group membership, it should layer an
explicit membership protocol (or a future library helper) on top of these
barrier semantics.
