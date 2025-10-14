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
   removes the caller from in-flight barriers, and returns a **reply-ring watermark**
   (`m_out_rep_c`) for the caller that covers all messages accepted so far.
   The coordinator separately computes per-recipient tokens when emitting
   completions; the return value is a single marker for the caller to flush.
   Remote processes flush that sequence before proceeding, guaranteeing
   that all outstanding traffic is visible before teardown continues.
   *(Coordinator::begin_process_draining in coordinator_impl.h)*
2. **Unpublish under normal communication.** With the coordinator aware of the
   shutdown and the channel quiesced, the process deactivates handlers and
   unpublishes its transceivers before pausing the message readers. This keeps
   the synchronous unpublish path free of shutdown races.
3. **Pause and destroy.** The process switches its readers to service mode,
   completes teardown, and releases all Sintra resources.

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
  resolves—either because the last active participant arrives or because every
  remaining pending member began draining—the coordinator sends the result to each
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

## Recent fixes and known issues

### Implemented fixes (2025-01)

Based on external code review, the following race conditions have been addressed:

**Fix 1: Per-recipient flush tokens from reply ring**
- **Problem:** Using a single global flush token from the request ring caused hangs
  when the token was ahead of some recipient's channel
- **Solution:** Moved to per-recipient flush tokens computed at message write time
  from the reply ring (`m_out_rep_c`). Each barrier completion message embeds a
  watermark that is valid specifically for that recipient's channel state.
- **Status:** ✅ Implemented in `begin_process_draining()` return path, barrier
  last-arrival return, and barrier completion emission loop

**Fix 2: Atomic barrier start with membership**
- **Problem:** Race between barrier membership snapshot and process removal/draining
  state changes could lead to inconsistent barrier sets
- **Solution:** Verified that `remove_process()` acquires `m_call_mutex`, ensuring
  barrier membership capture and draining filter are atomic with group changes
- **Status:** ✅ Verified atomic - both operations under same lock

**Fix 3: Draining bit lifecycle**
- **Problem:** Clearing draining bit during teardown created race windows where
  concurrent barriers could include a dying process
- **Solution:** Never clear draining bit during teardown. Only reset to ACTIVE when
  a new process is published into the same slot during recovery/restart.
- **Status:** ✅ Implemented - draining bit persists through teardown

**Fix 4: Deferred completion emission**
- **Problem:** Barrier completions need to be emitted outside the barrier lock to
  avoid re-entrancy deadlocks
- **Solution:** Use `run_after_current_handler()` to defer completion emission until
  after the current RPC handler returns. This queues the completion task for
  execution by the message reader thread.
- **Status:** ✅ Already implemented - `run_after_current_handler()` provides the
  required deferred execution mechanism

**Fix 5: Request-side flush race**
- **Problem:** Both request and reply reader threads were draining `m_flush_sequence`,
  causing premature barrier release when request ring advanced past flush_sequence
  before reply ring
- **Solution:** Removed flush token consumption from request reader. Only reply reader
  processes barrier flush tokens since completions travel on reply ring.
- **Status:** ✅ Implemented - removed from Process_message_reader::request_reader_function in process_message_reader_impl.h

**Fix 6: Shutdown-aware teardown**
- **Problem:** `Transceiver::destroy()` asserted on `rpc_unpublish_transceiver` failures
  when coordinator already shut down, causing Windows MessageBox dialogs
- **Solution:** Wrapped unpublish RPC in try-catch, treating failures as non-fatal
  during teardown
- **Status:** ✅ Implemented - try-catch added to Transceiver::destroy in transceiver_impl.h

**Fix 7: Watchdog pattern for finalize() RPC (CRITICAL)**
- **Problem:** Previous implementation used detached thread with lambda capturing locals
  by reference (`[&]`), creating UAF when timeout occurred and stack locals went out
  of scope while detached thread still running
- **Solution:** Replaced with watchdog pattern:
  - RPC runs synchronously on main thread (no capture issues)
  - Watchdog thread monitors with 5-second deadline
  - On timeout, watchdog calls `s_mproc->unblock_rpc()` to cancel hung RPC
  - Watchdog always joined (never detached)
- **Impact:** Eliminates UAF vulnerability, ensures deterministic shutdown, uses
  Sintra's built-in cancellation mechanism
- **Status:** ✅ Implemented - watchdog thread pattern in finalize() in sintra_impl.h

### Resolved issues

**MAJOR PROGRESS (2025-01):** All critical shutdown races have been fixed! Tests now
pass reliably with proper barrier synchronization, clean finalize() shutdown, and no
hangs or assertion dialogs.

**Previous Issue 1 - Flush ring mismatch (RESOLVED 2025-01):**
- Barrier completions travel on reply ring but code was checking request ring
- Fixed by using reply ring sequences throughout flush mechanism

**Previous Issue 2 - Request-side flush race (RESOLVED 2025-01-14):**
- Both readers draining `m_flush_sequence` caused premature barrier release
- Fixed by removing request-side consumption

**Previous Issue 3 - Shutdown ordering race (RESOLVED 2025-01-14):**
- **Root cause:** When main process called `finalize()` → `unpublish_all_transceivers()`,
  it attempted RPC to coordinator's `unpublish_transceiver`. If coordinator process had
  already started shutdown (`m_accepting_rpc_calls = false`), the RPC threw
  "Attempted to call an RPC on a target that is shutting down", triggering terminate handler.
- **Impact:** Tests timed out waiting for user to dismiss assertion dialog (Windows MessageBox).
  Tests WERE completing their work, but failing during cleanup.
- **Evidence:** Debugger stack traces showed main thread in win32u!NtUserWaitMessage →
  MessageBox → abort → custom_terminate_handler, called from
  `rpc_impl<unpublish_transceiver>` during `finalize()`.
- **Resolution:** Made `Transceiver::destroy()` shutdown-aware by wrapping the unpublish
  RPC in a try-catch block. RPC failures during shutdown are now treated as non-fatal,
  preventing the assertion dialog.

**Previous Issue 4 - UAF in finalize() (RESOLVED 2025-01-14):**
- Detached thread with `[&]` lambda capture caused UAF when timeout occurred
- Fixed with watchdog pattern using `unblock_rpc()` for clean cancellation

**Current Status (2025-01-14):**
- ✅ All critical tests passing (basic_pubsub, barrier_flush, barrier_stress)
- ✅ No hangs, no crashes, no assertion dialogs
- ✅ Deterministic shutdown with proper resource cleanup
- ✅ Documentation aligned with implementation

**Summary of All Fixes:**
1. ✅ Flush ring mismatch - reply ring sequences throughout
2. ✅ Per-recipient flush tokens - computed at write time in emit loop
3. ✅ Atomic barrier start - membership and draining filter under same lock
4. ✅ Draining bit lifecycle - persistent through teardown, reset on republish
5. ✅ Request-side flush race - removed from request reader
6. ✅ Shutdown-aware teardown - try-catch on unpublish RPC
7. ✅ Watchdog pattern - safe RPC timeout without UAF
