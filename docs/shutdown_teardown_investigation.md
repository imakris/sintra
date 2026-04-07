# Shutdown and Teardown Investigation

Date: 2026-04-07 / 2026-04-08

## Summary

A refactoring of the sintra shutdown protocol exposed several classes of
teardown races in multi-process tests and the library itself.  Four fixes
were committed to master.  One deeper issue remains: workers can get stuck
in ring-level IPC waits when a peer exits, preventing them from ever
reaching `shutdown()`.

---

## Problem 1: Workers skipping collective shutdown barriers

**Commits:** `d83c89b`, `7a88b91`

### Root cause

`shutdown()` had an early-return path (`if (!s_coord) return finalize_impl()`)
that sent every non-coordinator process straight to raw teardown, bypassing
the collective barriers.  The coordinator's processing-fence barrier then
waited for participants that would never arrive, causing `rpc_cancelled` to
escape to `std::terminate`.

### Fix (runtime.h, barrier.h)

- Changed the early-return condition to `if (!s_coord && s_coord_id == invalid_instance_id)`,
  so only true single-process runtimes skip barriers.  Workers with a remote
  coordinator now participate in collective shutdown.
- `shutdown_group_is_trivial()` similarly updated to return false for workers.
- `should_treat_rpc_failure_as_satisfied()` widened to also return true when
  `s_shutdown_state != idle`, as a safety net for edge-case RPC failures
  during the shutdown barrier sequence.
- Wrapped the collective protocol section in a catch-all that calls
  `finalize_impl()` before rethrowing, so `s_shutdown_state` is always reset
  on unexpected exceptions.

---

## Problem 2: Handler mutex starvation on Windows

**Commit:** `7a88b91`

### Root cause

`dispatch_event_handlers()` held `m_handlers_mutex` for the entire function,
including during user callback invocation.  On Windows, `CRITICAL_SECTION`
(backing `std::recursive_mutex`) has unfair locking — the most recent
releaser is favored.  When a reader thread rapidly processed many messages
(e.g. 150 ping-pong exchanges), it starved the main thread trying to
register a handler via `activate_slot()` / `receive<T>()`.

In `ping_pong_multi_test`, this caused `Stop` messages to be dropped because
`receive<Stop>()` could not register its handler in time.

### Fix (process_message_reader_impl.h)

Release `m_handlers_mutex` before invoking callbacks.  The handler vector is
already copied into a local variable, so the lock is only needed during the
lookup/copy phase.

---

## Problem 3: Uncaught exceptions at thread boundaries

**Commit:** `eca848f`

### Root cause

Several thread entry points and user-provided callbacks in the library had
no exception protection.  When a peer died, the resulting `rpc_cancelled`
(or `std::logic_error` from `Process_group::barrier()`) propagated to
`std::terminate`, killing the process and cascading through the swarm via
lifeline detection.

### Fix (logging.h, managed_process_impl.h, coordinator_impl.h)

Introduced `detail::exception_boundary` — a lightweight RAII wrapper that
catches and logs exceptions at thread/callback boundaries.  Applied to all
unprotected boundaries:

- Worker entry function (`go()` → `m_entry_function`)
- Signal dispatch threads (Windows and POSIX)
- Lifeline watch threads (Windows and POSIX)
- Recovery runner thread
- Lifecycle event handler callback
- Recovery policy callback
- Post-handler callback in the message reader loop

Logging uses `warning` level because these exceptions are expected during
normal shutdown cascades (a peer died, so your RPC got cancelled).

---

## Problem 4: Uncoordinated test teardown

**Commit:** `0a639a5`, `55bc7f1`

### Root cause

`run_multi_process_test()` called `detail::finalize_impl()` directly,
without collective barriers.  After a test's final user barrier completed,
all processes raced into `finalize_impl()` independently.  A fast worker
could begin draining (unblocking RPCs, unpublishing transceivers) while
the coordinator was still unwinding from its barrier return.

### Fix (test_utils.h)

- `run_multi_process_test()` now delegates to `run_multi_process_shutdown_test()`,
  which uses `shutdown()` for coordinated collective teardown.
- Added `run_multi_process_test_raw()` with the old `finalize_impl()`
  behaviour for two tests that deliberately break processes
  (`barrier_guardrails_test`, `lifecycle_handler_test`).

---

## Problem 5: Barrier exception type gap

**Commit:** `7a88b91`

### Root cause

`rendezvous_barrier()` caught `rpc_cancelled` and `std::runtime_error`, but
`Process_group::barrier()` can throw `std::logic_error` ("The caller is not
a member of the process group") which inherits from `std::exception`, not
`std::runtime_error`.  The exception escaped uncaught.

### Fix (barrier.h)

Added a `catch (const std::exception& e)` block after the existing handlers,
with the same `should_treat_rpc_failure_as_satisfied()` logic.

---

## Problem 6: Examples using removed API

**Commit:** `7a88b91`

`sintra::finalize()` was moved to `sintra::detail::finalize()` during the
refactoring.  All examples migrated to `sintra::shutdown()`.

---

## Remaining issue: Ring-level blocking on peer exit

### Symptom

`join_swarm_midflight_test` intermittently times out (30 s) on macOS CI.
The process is stuck in `Ring_R::wait_for_new_data()` (rings.h:2012),
waiting on a semaphore for data from a ring whose writer has exited.

### What was tried and rejected

**Per-test final barriers:** Adding `sintra::barrier("shutdown-ready",
"_sintra_all_processes")` before `shutdown()` in every test.  All 7
modified tests deadlocked because:

1. The barrier cardinality doesn't always match (dynamic processes from
   `join_swarm()`, crashed processes, etc.).
2. `validate_no_barrier_during_shutdown()` explicitly prohibits user
   barriers on `_sintra_all_processes` during an active shutdown protocol.
3. Worker process functions that are stuck in IPC waits never reach the
   barrier, creating the same hang the barriers were supposed to prevent.

**Auto-shutdown in `init()` / `go()`:** Making the library automatically
call `shutdown()` after the worker's entry function returns.  Rejected
because:

- It breaks the architectural contract: workers may do work after
  `init()` returns and before calling `shutdown()`.
- The `shutdown_protocol_state` compare-exchange-strong prevents
  double-shutdown by throwing `std::logic_error`, which would break
  every test that calls `shutdown()` in its own `main()`.
- It doesn't help when the worker is stuck inside the entry function
  (the auto-shutdown would never run).

### Analysis

The draining mechanism already handles the case where a worker crashes:

1. Lifeline break detected → `unpublish_transceiver()` called on coordinator
2. `m_draining_process_states[slot] = 1` (atomic, before barrier cleanup)
3. `drop_from_inflight_barriers()` removes the dead process from pending
   barriers and auto-completes them if all remaining participants have
   arrived.
4. New barriers created after the process is marked as draining filter it
   out at creation time (coordinator_impl.h:62–73).

This should mean that if a worker dies while a shutdown barrier is pending,
the barrier auto-completes.  The 30-second timeout in CI suggests either:

- The detection path (lifeline → unpublish → barrier completion) is too
  slow on macOS under load, or
- The worker hasn't actually crashed — it's alive but stuck in
  `Ring_R::wait_for_new_data()`, so the lifeline never breaks, the
  coordinator never unpublishes it, and the barrier never completes.

The second scenario is more likely: the worker process is alive (the pipe
is open), but its ring reader thread is blocked on a semaphore waiting
for data from a peer that has already exited cleanly.  Because the peer
exited (not crashed), the ring writer's side of shared memory may still
look valid — no broken pipe, no signal, no lifeline event — but no new
data will ever arrive.

### Possible fixes (not yet implemented)

**Option A: Timeout on the collective barrier in `shutdown()`.**
Add a deadline to the `internal_processing_fence_barrier()` call at
runtime.h:493.  If the barrier doesn't complete within (e.g.) 10 seconds,
log a warning and fall back to `finalize_impl()` with degraded semantics.
This mirrors the existing 20-second timeout in `shutdown_coordinator_drain_wait()`
and the 5-second timeout for the draining RPC in `finalize_impl()`.

Risk: a timed-out barrier breaks the collective contract.  But
`should_treat_rpc_failure_as_satisfied()` already treats shutdown-state
RPC failures as satisfied, so timing out is semantically consistent.

**Option B: Ring reader awareness of peer exit.**
When a ring writer exits (cleanly or not), ensure the ring reader's
semaphore is posted / unblocked.  This would let the blocked reader
thread detect the writer is gone and return an error rather than waiting
forever.  This is the most correct fix but requires changes to the
ring/semaphore layer.

**Option C: `unblock_rpc()` during shutdown.**
In `shutdown()`, before entering the collective barrier, call
`s_mproc->unblock_rpc()` for any peers that have started draining.
This would wake workers stuck in RPC waits.  However, this only helps
if the stuck process is in an RPC wait, not a raw ring read.

### Recommendation

Option B (ring reader awareness of peer exit) is the correct long-term
fix.  Option A (barrier timeout) is a reasonable short-term safeguard
that prevents indefinite hangs.  Both could be implemented together.

---

## Test inventory: shutdown paths

Tests using `run_multi_process_test` (delegates to `shutdown()`):
barrier_complex_choreography_test, barrier_flush_test, basic_pub_sub,
complex_choreography_test, lifecycle_handler_test (via `_raw`),
barrier_guardrails_test (via `_raw`), ping_pong_multi_test,
processing_fence_test, rpc_append_test, rpc_async_lifecycle_test.

Tests with custom `main()` calling `shutdown()` directly:
choreography_extreme_test, barrier_rapid_reuse_test,
barrier_pathological_choreography_test, extreme_choreography_test,
barrier_stress_test, complex_choreography_stress_test,
locality_messaging_test, receive_test, join_swarm_midflight_test.

The second group is vulnerable to the ring-level blocking issue if a
worker gets stuck before reaching `shutdown()`.
