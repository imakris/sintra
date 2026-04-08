# Public `leave()` API Design

## Summary

Sintra currently has two real lifecycle shapes:

1. **Collective terminal shutdown**
   All live participants enter a shared terminal protocol.
   Public API today: `sintra::shutdown()` and `sintra::shutdown(shutdown_options{...})`

2. **Local graceful departure**
   One process intentionally leaves while the rest of the system continues.
   Public API today: none
   Actual implementation today: `sintra::detail::finalize()`

That second shape is not accidental. It is already implemented, already relied
on by examples, and already semantically distinct from coordinated shutdown.
The gap is that the public API does not acknowledge it directly.

This document proposes making that second lifecycle shape first-class with a
new public API for the supported scope where unilateral departure is actually
well-defined:

```cpp
bool leave();
```

## Problem

The current public surface teaches an awkward and misleading story:

- `shutdown()` is the standard public terminal API
- some legitimate user-facing examples need different semantics
- those examples currently fall back to `sintra::detail::finalize()`

That creates the wrong impression for users:

- either the library is incomplete
- or the documented public lifecycle is not the real lifecycle model

Both impressions are correct enough to be harmful.

The Qt examples make the problem obvious. A window may close normally while:

- the coordinator stays alive
- sibling windows stay alive
- lifecycle handlers still observe a `normal_exit`
- recovery logic must not restart the closed window

That is not collective shutdown. It is a clean unilateral departure.

Trying to force that case into `shutdown()` is structurally wrong, because
`shutdown()` currently means:

- all currently live participants enter the same terminal protocol
- the runtime owns the collective handoff
- internal `_sintra_all_processes` coordination is part of the contract

A single window closing while the rest of the system continues is a different
operation.

## Aims And Objectives

The design should satisfy these objectives:

1. Keep `shutdown()` as the standard collective terminal API.
2. Provide a clear, public, intent-bearing API for unilateral clean departure.
3. Remove the need for ordinary leaf-participant applications and examples to
   call `sintra::detail::finalize()`.
4. Reuse the existing local drain-and-unpublish machinery where it is already
   correct, while making any narrowed coordinator semantics explicit.
5. Keep the public lifecycle vocabulary protocol-shaped rather than
   implementation-shaped.
6. Avoid overloading `shutdown()` with mutually incompatible semantics.
7. Keep low-level raw teardown available in `detail` for tests, experiments,
   and deliberate primitive-level work.

## Non-Goals

This proposal does not try to:

- replace `shutdown()`
- fold all lifecycle operations into one function with many flags
- expose barrier names or internal protocol phases
- redesign recovery, lifeline ownership, or barrier semantics
- eliminate the low-level `detail::finalize()` escape hatch for tests and
  exceptional paths

## Why This Does Not Belong In `shutdown_options`

The current direction for shutdown says:

- extend `shutdown_options` when the operation is still semantically just
  shutdown
- add a separate public helper when it is no longer semantically just shutdown

This case is no longer semantically just shutdown.

If `shutdown()` gained an option like:

```cpp
shutdown(shutdown_options{ .local_only = true });
```

then one API name would mean two incompatible things:

- collective terminal protocol for all live participants
- unilateral local departure while peers continue running

That would make the API less clear, not more clear.

The caller should not have to ask:

- "Is `shutdown()` ending the whole Sintra topology?"
- "Or is it only ending this participant?"

Those are different verbs.

## Candidate Public API Shapes

### Option A: `bool leave();`

Example:

```cpp
int main()
{
    sintra::init(argc, argv);
    run_window();
    sintra::leave();
}
```

Pros:

- short
- intent-bearing
- distinct from collective shutdown
- easy to teach
- matches the actual lifecycle meaning

Cons:

- introduces a second public lifecycle verb

### Option B: `bool depart();`

Pros:

- also distinct from shutdown
- clearly non-collective

Cons:

- less natural than `leave()`
- weaker for examples and docs

### Option C: `bool shutdown_local();`

Pros:

- technically explicit

Cons:

- still overloads `shutdown` vocabulary
- suggests this is just a mode of coordinated shutdown
- keeps the semantic confusion alive

### Option D: make `finalize()` public again

Pros:

- maps closely to the existing implementation

Cons:

- implementation-shaped rather than intent-shaped
- teaches teardown machinery instead of lifecycle semantics
- keeps the current "borderline hack" feeling

## Recommendation

Adopt:

```cpp
bool leave();
```

and document the lifecycle split explicitly:

- `shutdown()` means the participating runtime is ending collectively
- `leave()` means this process is exiting cleanly while peers may continue

`leave()` should become the public API used by:

- long-lived multi-process UI examples where individual windows may close
- applications with coordinator-observed normal exit of one participant
- processes that intentionally withdraw from the swarm without ending it, as
  long as they are not relying on parent-owned descendants to remain alive

`detail::finalize()` remains available only as a low-level escape hatch.

## Proposed Public Semantics

### `bool leave();`

Meaning:

- announce that this process is intentionally draining
- use the existing local drain-and-unpublish teardown path
- tear down the local Sintra runtime without entering collective shutdown

What it guarantees:

- if the draining announcement reaches the coordinator successfully, the
  coordinator can observe the exit as intentional draining and therefore report
  `normal_exit` rather than `crash`
- under healthy coordinator connectivity, this process is excluded from future
  barriers and released from in-flight barrier participation through the normal
  draining path
- peers that are not owned descendants of the leaving process may continue
  running

Initial supported scope:

- ordinary worker / participant processes that do not own spawned descendants
- single-process runtimes
- coordinator only when it is already the sole remaining known process from the
  runtime's point of view

What it does not mean:

- peers are shutting down
- any collective `_sintra_all_processes` barrier has run
- the topology is globally quiescent
- child processes owned by the leaving process will remain alive after the
  owner exits; existing lifeline semantics still apply
- the library automatically provides application-specific exit reasons beyond
  the runtime-level `normal_exit` / draining signal

### Failure behavior

`leave()` should follow the same broad style as the existing local drain path:

- return `true` when the local draining teardown completes
- return `false` if no runtime is active
- throw `std::logic_error` for illegal lifecycle composition or unsupported
  coordinator usage
- degrade conservatively when coordinator communication is already gone, using
  the same best-effort local teardown path that exists today; in that degraded
  path, `normal_exit` and coordinator-observed draining are no longer
  guaranteed

Coordinator-specific side effect:

- when the coordinator is allowed to call `leave()` because it is already the
  sole remaining participant, the existing finalize path still calls
  `begin_shutdown()` internally, which cancels any active recovery work
  associated with that coordinator

Threading expectation:

- `leave()` should be documented as a top-level control-thread operation
- it must not be called from within a message handler callback
- concurrent lifecycle API calls from different threads are guarded by the
  protocol-state CAS, but ordinary usage should still treat teardown initiation
  as a single-threaded control decision

## Recommended Documentation Story

The public lifecycle section should say:

### Collective end

```cpp
sintra::shutdown();
sintra::shutdown(sintra::shutdown_options{...});
```

Use when:

- all live participants are ending together
- the runtime should own the final collective handoff

### Local departure

```cpp
sintra::leave();
```

Use when:

- this process is intentionally exiting
- peers may continue running
- the coordinator should still observe a clean normal exit

### Low-level escape hatch

```cpp
sintra::detail::finalize();
```

Use only for:

- tests that deliberately exercise primitive behavior
- exceptional/error paths
- direct low-level runtime work

## Implementation Details

## 1. Public facade

Add this declaration to `include/sintra/sintra.h` near the lifecycle API:

```cpp
/// Perform a clean local departure without entering collective shutdown.
bool leave();
```

Documentation should explicitly contrast it with `shutdown()`.

## 2. Runtime implementation

The implementation should live in `include/sintra/detail/runtime.h`.

Recommended shape:

```cpp
inline bool leave()
{
    auto expected = detail::shutdown_protocol_state::idle;
    if (!detail::s_shutdown_state.compare_exchange_strong(
            expected,
            detail::shutdown_protocol_state::local_departure_entered,
            std::memory_order_acq_rel))
    {
        throw std::logic_error(
            "sintra::leave() called while another lifecycle teardown is already in progress "
            "(state=" + std::to_string(static_cast<int>(expected)) + ").");
    }

    if (!s_mproc) {
        detail::s_shutdown_state.store(
            detail::shutdown_protocol_state::idle,
            std::memory_order_release);
        return false;
    }

    // leave() is a unilateral participant departure, not coordinator-owned
    // topology shutdown. The coordinator may only use it when it is already
    // the sole remaining known process according to the same candidate set
    // that coordinator finalize waits on.
    if (s_coord && !detail::coordinator_can_leave_now()) {
        throw std::logic_error(
            "sintra::leave() is not supported on a coordinator while other participants "
            "are still alive or otherwise still known to the coordinator. "
            "Use shutdown() for collective termination.");
    }

    return detail::finalize_impl();
}
```

Rationale:

- the current finalize path already performs the right local draining and
  teardown choreography, but its state transition handling is too weak to
  expose directly as a new public API contract
- the public API still needs to claim its own lifecycle transition atomically
  before entering that machinery
- a dedicated protocol state makes `leave()` visible in diagnostics and
  distinct from raw `detail::finalize()`
- coordinator eligibility must use the same notion of "known remaining
  processes" that coordinator-local finalize already waits on, not just
  `_sintra_all_processes` membership
- the current `finalize_impl()` machinery can remain the shared teardown
  worker as long as public entry points claim lifecycle state before entering
  it and reset state correctly on early-return and exception paths
- `detail::finalize()` can remain as the low-level name, while `leave()` is the
  public intent-bearing name over the same machinery

## 3. Lifecycle guardrails

`leave()` should reject the same illegal composition that `detail::finalize()`
already rejects when a collective shutdown protocol is active, while also
claiming its own state atomically and without leaving the runtime stranded in a
non-idle state on early-return paths.

That means:

- `leave()` must fail fast if `shutdown()` is already in progress
- `leave()` must fail fast if another `leave()` / `finalize()`-shaped teardown
  is already in progress
- `shutdown()` should continue to own the collective terminal protocol state
- callers should not mix `leave()` with `shutdown()` in one active runtime
- `leave()` must reject coordinator-owned unilateral departure while other
  non-self processes are still known to the coordinator

Recommended internal state extension:

- add `local_departure_entered` alongside the existing collective-shutdown
  states
- have `leave()` CAS from `idle` to `local_departure_entered`
- keep `finalize_impl()` as the shared teardown worker, with thin
  public/detail entry points that claim/reset lifecycle state around it
- align `detail::finalize()` to the same state-claiming discipline so the old
  low-level race is not preserved next to the new public API

This keeps the public API protocol-shaped and resolves the `leave()` /
`shutdown()` race that a load-only check would leave open.

## 4. Existing low-level function names

Keep these internal names:

- `detail::finalize()`
- `detail::finalize_impl()`

Reason:

- tests and low-level runtime code already refer to them
- the point of this proposal is not to remove primitive access
- the point is to stop forcing ordinary users onto primitive names

## 5. Example migration

After `leave()` exists, migrate only the call sites that are truly ordinary
non-collective participant exits:

- `example/qt_basic/cursor_sync_receiver.cpp`: migrate normal exit to `leave()`
- `example/qt_multi_cursor/multi_cursor_window.cpp`: migrate normal exit to
  `leave()`
- related README text for those examples

Do **not** migrate every current `detail::finalize()` call site blindly.

Per-call-site guidance:

- `example/qt_basic/cursor_sync_sender.cpp`, spawn-failure cleanup:
  keep `detail::finalize()` because it is an error-path / setup-failure escape
  hatch, not an ordinary unilateral departure
- `example/qt_basic/cursor_sync_sender.cpp`, normal window close:
  do not migrate to `leave()` in v1, because the sender is the coordinator
  and owner of a still-live receiver via lifeline semantics; that is outside
  the supported unilateral-leave scope and needs either example redesign or a
  different topology-ownership feature
- `example/qt_multi_cursor/multi_cursor_coordinator.cpp` after all windows have
  already exited:
  use `shutdown()` by convention because the coordinator is ending the remaining
  topology; note that this reaches the trivial-group fast path, so it is a
  semantic choice rather than a second collective barrier round

Important limitation:

- this proposal does **not** by itself eliminate all ordinary-example uses of
  `detail::finalize()` in every shipped topology
- specifically, parent-owned descendant topologies like `qt_basic` still need
  redesign if the goal is to remove detail-level teardown from all examples

## 6. Test plan

Add focused tests for the public API shape, not just the underlying primitive.

### Unit / lifecycle guardrail coverage

- `leave()` without init returns `false`
- `leave()` after `init()` succeeds in single-process mode
- `leave()` rejects for every non-idle lifecycle teardown state, mirroring the
  rigor of `lifecycle_guardrails_test.cpp`
- `leave()` rejects on a coordinator while other non-self processes are still
  known to the coordinator
- `leave()` succeeds on a coordinator once it is already the sole remaining
  known process
- a second `leave()` after successful teardown returns `false`
- `shutdown()` after successful `leave()` returns `false` because no runtime is
  active
- `leave()` and `shutdown()` racing from different threads do not both enter
  teardown

### Multi-process behavior coverage

- one worker calls `leave()` while coordinator and sibling workers continue
- under healthy coordinator connectivity, coordinator emits `normal_exit`
  rather than `crash`
- remaining peers continue messaging and barriers correctly after the leaver is
  removed from participation
- a worker that enabled recovery and then calls `leave()` does **not** trigger
  recovery
- a worker can call `leave()` while barrier participation is being updated
  without deadlocking or corrupting remaining barrier membership
- degraded coordinator connectivity path:
  a worker still completes `leave()` without hanging even if the coordinator
  never observes draining
- owner/descendant behavior:
  if a leaving process owns spawned children, test and document that current
  lifeline semantics still terminate those descendants
- introduce a mixed-topology helper (for example `run_multi_process_leave_test`)
  so one or more participants can call `leave()` while the rest later call
  `shutdown()`

### Example-shape coverage

- a Qt-like topology test where one child exits intentionally and the rest stay
  alive
- validate explicitly that:
  - the lifecycle event is `normal_exit`
  - remaining peers continue messaging normally
  - the coordinator can still finish cleanly afterward
- confirm that the public example surface can use `leave()` instead of a detail
  API where the topology semantics actually match unilateral departure

## 7. Documentation updates

Update:

- `include/sintra/sintra.h`
- `docs/barriers_and_shutdown.md`
- `docs/process_lifecycle_notes.md`
- `include/sintra/detail/runtime.h` diagnostics and error strings that enumerate
  public teardown choices

Key messaging:

- `shutdown()` is the collective terminal API
- `leave()` is the clean unilateral departure API
- `detail::finalize()` is low-level

## Why This Is Better Than The Current Situation

This proposal improves the API in four ways:

1. It acknowledges the real lifecycle model already implemented in the
   runtime.
2. It gives examples a proper public API instead of a detail namespace escape.
3. It keeps `shutdown()` semantically narrow and predictable.
4. It avoids inventing a flag-heavy `shutdown()` that means contradictory
   things depending on options.
5. It makes the local-departure path explicit in the lifecycle state machine
   instead of hiding it behind the same state used by raw finalize.
6. It makes the ownership limit explicit: leaving a process does not magically
   detach descendants that are currently tied to it by lifelines.

## Open Questions

### 1. Should `leave()` eventually gain options?

Not initially.

The initial version should stay minimal. If a future requirement appears for
local-departure-specific customization, that should be evaluated separately
under a new `leave_options` type rather than pushed into `shutdown_options`.

### 2. Should `leave()` be allowed from all processes, including the coordinator?

Not in the general case.

Today the coordinator owns topology-level responsibilities:

- it owns lifelines for spawned participants
- it waits for all draining in coordinator-local finalize
- its exit can force child termination indirectly

So coordinator `leave()` is only coherent when the coordinator is already the
sole remaining known process. That case is supported by the proposed stronger
guard.

Supporting "coordinator leaves while peers keep running" is a different
feature, likely requiring ownership transfer or a detached-topology model, and
is out of scope for this proposal.

### 3. Should public `leave()` replace all current `detail::finalize()` users?

No.

Some tests and exceptional paths should remain deliberately low-level.
The migration target is ordinary application and example code that is using the
detail namespace only because no public unilateral-departure API exists.
Topologies that depend on owned descendants surviving the leaver are not in
scope for this first public `leave()` surface.

## Final Recommendation

Add a new public API:

```cpp
bool leave();
```

Make it the first-class public representation of "this process exits cleanly
without entering collective shutdown, in the supported cases where unilateral
departure is actually well-defined."

Keep:

- `shutdown()` for collective end
- `leave()` for unilateral clean departure
- `detail::finalize()` for low-level and test-only primitive work

That is the clearest public lifecycle story the current architecture can
support without distorting `shutdown()` semantics or pretending that
coordinator departure is already a solved unilateral-leave case.
