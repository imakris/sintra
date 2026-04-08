# Barriers and shutdown semantics

Sintra distinguishes three lifecycle states for processes that participate in
coordinated execution:

- **ACTIVE** - the process contributes to barriers and other group-wide RPCs.
- **DRAINING** - the process is shutting down. It is excluded from future
  barriers and automatically released from any barrier that is already in
  progress.
- **TERMINATED** - the process has exited and its resources have been scavenged.

## `sintra::shutdown()`

`shutdown()` is the standard terminal API for the multi-process lifecycle.
All live participants call `shutdown()` and the runtime owns the collective
handoff and internal synchronization.

### Shape 1: simple collective shutdown

```cpp
sintra::shutdown();
```

This performs a `processing_fence_t` barrier on `"_sintra_all_processes"` and
then tears down the local Sintra runtime. On the coordinator, the helper waits
for the rest of the shutdown group to unpublish itself before finalizing
locally, so peers do not lose their lifeline owner mid-teardown.

### Shape 2: coordinator-side shutdown hook

```cpp
sintra::shutdown(
    sintra::shutdown_options{
        .coordinator_shutdown_hook = [&] {
            write_summary_file(path);
        }
    });
```

All participants call `shutdown(options)`. After the collective processing
fence, the coordinator runs the hook while non-coordinator processes wait
inside the standard shutdown path. The runtime owns the necessary internal
synchronization. The hook is coordinator-local and must not initiate new peer
coordination, extra barriers, or additional custom protocol steps.

## `sintra::leave()`

`leave()` is the public API for a clean local departure when this process is
exiting but peers may continue running.

```cpp
sintra::leave();
```

Use `leave()` when:

- one participant exits intentionally
- the rest of the topology should continue
- the process is a leaf-like participant that is not relying on owned
  descendants to outlive it
- the call is made from a top-level control thread rather than a message
  handler or post-handler callback

On healthy coordinator connectivity, `leave()` follows the same local drain,
pause, unpublish, and teardown choreography as the internal finalize path, but
it does not enter the collective `_sintra_all_processes` shutdown protocol.
The coordinator may call `leave()` only when it is already the sole remaining
known process from the runtime's point of view.

### Extension rule

Supported non-happy-path shutdowns should be expressed through additional
fields in `shutdown_options`, rather than a parallel family of helper names.
Introduce a separate public helper only if the operation is no longer
semantically just shutdown.

### Protocol state and fail-fast guardrails

The runtime tracks the active lifecycle teardown state (in
`detail::shutdown_protocol_state`, not part of the public API):

- `idle` - no teardown in progress
- `collective_shutdown_entered` - `shutdown()` has been called
- `local_departure_entered` - `leave()` has been called
- `coordinator_hook_running` - coordinator hook is executing
- `coordinator_hook_completed` - hook finished, awaiting peer synchronization
- `finalizing` - raw teardown in progress

Illegal compositions are rejected immediately:

- Calling `shutdown()` while another shutdown is already in progress
- Calling `leave()` while another lifecycle teardown is already in progress
- Calling `detail::finalize()` while a lifecycle teardown is active
- Calling a user-facing barrier on `_sintra_all_processes` while a lifecycle
  teardown is active

### Notes

Barrier names beginning with `"_sintra_"` are reserved for internal runtime
protocols. User code should not reuse them.

`shutdown()` is not meant to replace every explicit final barrier pattern.
When a workflow needs a stronger algorithm-specific handoff, such as an
application-defined final rendezvous or a staged ownership transfer, keep that
protocol explicit and only enter `shutdown()` after it completes.

## `detail::finalize()` (low-level teardown)

`detail::finalize()` is the low-level teardown path for single-process
programs, exceptional/error paths, or test code that cannot participate in a
symmetric shutdown. It lives in the `sintra::detail` namespace — ordinary
multi-process callers should use `shutdown()` instead. The internal
implementation (`detail::finalize_impl()`) is also available for deliberate
low-level work in tests and experiments. Ordinary code that needs a clean
unilateral departure should use `leave()` instead of `detail::finalize()`.

Calling `detail::finalize()` performs the following steps:

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

## Barrier mode guardrails

- **Delivery fences are local-only:** `barrier()` / `barrier<delivery_fence_t>()`
  proves that this process drained its readers up to the barrier point. It does
  not add a second rendezvous proving that peers also finished draining.
- **Processing-fence phase names are internal:** the runtime now keeps the
  second `processing_fence_t` rendezvous out of the user barrier namespace.
- **Mixed barrier modes fail fast:** all participants in a given barrier round
  must use the same barrier mode. Mixing `rendezvous_t`, `delivery_fence_t`,
  and `processing_fence_t` on the same barrier name is rejected immediately
  instead of hanging in the hidden processing phase.

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

These rules eliminate shutdown deadlocks during teardown, but they do not turn
`detail::finalize()` into a collective "safe to shut down now" handshake by
itself. For ordinary multi-process completion, use `sintra::shutdown()` or
`sintra::shutdown(sintra::shutdown_options{...})`. For unilateral participant
departure, use `sintra::leave()`. If an algorithm needs stronger guarantees
about group membership than the default lifecycle helpers provide, it should
layer an explicit membership protocol on top of these barrier semantics.
