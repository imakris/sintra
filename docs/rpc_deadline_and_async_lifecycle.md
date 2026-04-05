# RPC Deadlines, Abandonment, and Async Lifecycle

This note explores whether Sintra should grow a first-class RPC deadline and
asynchronous wait model, and if so, what that model should mean precisely.

The motivating context came from higher-level systems built on top of Sintra
that currently use watchdogs and forced shutdown paths around synchronous RPC
stop sequences. The immediate question was whether Sintra should provide a
cleaner primitive for "I am no longer willing to wait for this RPC" without
claiming that the remote execution itself has stopped.

This document is intentionally a design note, not an implementation plan. The
goal is to clarify intent, identify the real problem, and describe a direction
that could be reviewed carefully before any broad-impact change.

## Intent

Sintra already has a strong synchronous request/reply model. That model is
useful and should remain the default. The intent here is not to replace it.

The intent is to make one additional lifecycle state explicit:

- the caller may stop waiting for a reply before the callee finishes

That is different from:

- cancelling the remote function
- interrupting a handler while it is executing
- proving that a timeout means the callee has stopped

The design goal is to make caller-side abandonment honest, first-class, and
usable without forcing higher-level libraries to build ad hoc watchdog logic.

## Objectives

Any Sintra-level change in this area should satisfy the following objectives:

1. Preserve the existing synchronous RPC programming model.
2. Avoid lying about cancellation semantics.
3. Make caller-side deadline handling explicit and easy to use.
4. Reuse existing Sintra request/reply infrastructure as much as possible.
5. Keep process-boundary escalation separate from RPC lifecycle tracking.
6. Avoid introducing unsafe preemptive interruption semantics.
7. Be broadly useful beyond a single downstream framework.

## Non-objectives

The following should not be goals of this work:

- preemptively stopping arbitrary remote code mid-handler
- claiming that deadline expiry implies remote execution ended
- providing thread interruption inside a process
- replacing process-level kill as the hard recovery boundary
- making all RPCs globally asynchronous by default

## Why this matters

In several real systems built on top of Sintra, shutdown currently involves:

1. issuing one or more synchronous stop/unload/shutdown RPCs
2. waiting in a background thread for those calls to return
3. using timers or watchdog threads to decide when the wait is no longer
   acceptable
4. escalating to process-level cleanup if the stop path appears stuck

That structure creates problems:

- watchdog logic is duplicated in application code
- timeout semantics are vague and easy to misinterpret
- session-layer shutdown state becomes coupled to transport waiting behavior
- destructors and manager shutdown code are tempted to block on stop threads

The actual generic need is simpler:

- let the caller stop waiting cleanly
- allow late replies to be discarded safely
- keep process liveness and RPC completion as separate concerns

## Existing Sintra infrastructure

Sintra already provides important pieces of this model.

### 1. Process-scoped outstanding RPC waits can already be unblocked

`Managed_process::unblock_rpc(...)` marks matching outstanding RPCs as
cancelled and wakes their waiters.

Relevant code:

- `include/sintra/detail/process/managed_process_impl.h`
- `include/sintra/detail/transceiver_impl.h`

This is already used in some teardown paths, but it is not yet a general
caller-side abandonment primitive. In particular, the current implementation is
process-scoped and should not be described as if it already provides a complete
"cancel any outstanding wait" lifecycle model.

### 2. Synchronous RPC already has a caller-side wait state

`Transceiver::rpc_impl(...)` already materializes a per-call `Rpc_state`,
registers return handlers, waits on a condition variable, and distinguishes:

- normal return
- remote exception
- cancelled wait via `rpc_cancelled`
- a generic fallback failure path

This is an excellent starting point because the lifecycle state already exists,
even though it is currently exposed only through the synchronous API.

It is important not to overstate this: the current internal state does not yet
carry a first-class `deadline_exceeded` or `transport_failed` classification.

### 3. Receiver-side RPC shutdown already exists

`Transceiver::ensure_rpc_shutdown()` already:

- stops accepting new RPC calls
- waits for active RPC handlers to finish

This is important because it shows Sintra already has a notion of RPC
lifecycle gating on the receiver side. That is separate from caller-side
abandonment, but complementary to it.

### 4. Sintra already acknowledges an async split point

There is an explicit note in `Transceiver::rpc_impl(...)` that an asynchronous
variant could split at the point where the request has been written and the
return handler is registered, then continue waiting via stored RPC state.

That means the design space is not foreign to the implementation. The library
already has a promising transported-RPC seam.

That seam does not currently cover every existing RPC call path. Same-process
non-strict RPCs can still take a direct-call fast path that bypasses
`Rpc_state`, return-handler registration, and outstanding-RPC tracking. Any
async/deadline API must define explicitly how local same-process targets behave.

## The key semantic distinction

This entire topic depends on one rule:

> A caller timeout must mean only that the caller stopped waiting.

It must not mean:

> the remote handler stopped executing

Those are fundamentally different events.

This distinction is the difference between a sound design and a misleading one.

## Why true cancellation is hard

There are three broad models:

### Caller-side timeout

The caller stops waiting after a deadline.

Properties:

- easy to implement cleanly
- does not affect remote execution
- safe and honest

### Cooperative cancellation

The caller signals cancellation and the callee checks for it voluntarily.

Properties:

- can work well when handlers are structured for it
- requires explicit application participation
- still does not provide immediate interruption guarantees

### Preemptive cancellation

The system interrupts or aborts executing handler code from outside.

Properties:

- unsafe inside a process
- difficult to make correct across arbitrary user code
- usually only tolerable at process boundaries

For Sintra, the first model is the right general primitive. The second may
become useful later for specific application-level patterns. The third should
not be a design target.

## Proposed direction

The most promising addition would be:

- a first-class asynchronous RPC handle API
- optional deadline-based waiting on that handle
- explicit caller-side abandonment

This would not change the core meaning of RPC. It would expose the lifecycle
that Sintra already has internally.

### Sketch of the intended public model

Illustrative only:

```cpp
auto handle = Some_service::rpc_async_method(target, arg1, arg2);

switch (handle.wait_until(deadline)) {
case Rpc_wait_status::completed:
    switch (handle.state()) {
    case Rpc_completion_state::returned:
        auto value = handle.get();
        break;

    case Rpc_completion_state::remote_exception:
        handle.get(); // rethrows
        break;

    case Rpc_completion_state::cancelled:
        break;

    case Rpc_completion_state::abandoned:
        break;
    }
    break;

case Rpc_wait_status::deadline_exceeded:
    handle.abandon();
    break;
}
```

The exact spelling should follow Sintra's existing generated `rpc_<method>(...)`
style rather than inventing an unrelated naming family.

## Why an async handle is better than a sync timeout wrapper

A synchronous timeout wrapper can be layered on top later if it proves useful,
but it is not the primitive that needs to be added first. The important
capability is not merely "wait for at most N milliseconds". The important
capability is:

- the caller can retain or drop interest in the eventual result
- late reply handling remains safe and explicit
- higher-level orchestration can decide whether to keep waiting, detach, or
  escalate

That is easier to reason about with a handle than with a one-shot wrapper.

## Internal implementation fit

This proposal fits the existing Sintra internals well.

### Existing pieces that can be reused

- `Rpc_state`
- outstanding RPC registration
- return handler registration
- deferral handling
- `rpc_cancelled`
- `Managed_process::unblock_rpc(...)`

### Existing behavioral baseline for late replies

The current reply dispatch path already drops replies safely when the caller-side
handler is gone or the receiver object has already disappeared locally.

That behavior is a useful baseline because it means abandonment does not require
inventing a brand-new late-reply policy from scratch. What still needs to be
specified is the exact point at which a handle may deactivate its handler and
how the shared state prevents false completion when a callback was already
copied before deactivation.

### Likely internal refactoring

Today, `rpc_impl(...)` constructs `Rpc_state` and then blocks until completion.

An async variant would instead:

1. construct and register `Rpc_state`
2. write the request
3. return a lightweight handle that owns/shared-owns the RPC state
4. let callers wait, try-wait, inspect state, or abandon
5. clean up return handlers and outstanding state when the lifecycle completes

The synchronous path could then reuse the same transported-state machinery:

- create transported handle state
- wait without deadline
- extract result

That would keep semantics centralized for transported RPCs without implying that
the existing local direct-call fast path must disappear.

The public contract should still stay explicit:

- `Rpc_handle` and `rpc_async_<method>()` are the async-facing surface
- `rpc_<method>()` remains a synchronous API
- any sync reuse of the handle/state machinery is an internal implementation
  detail, not a mixed caller model

## Terminal and wait states

The design should define a small explicit state machine.

### Handle terminal states

Recommended initial handle states:

- `pending`
- `returned`
- `remote_exception`
- `cancelled`
- `abandoned`

`deadline_exceeded` is not a handle terminal state by itself. It is the result
of a specific bounded wait attempt. A caller may receive `deadline_exceeded`
from `wait_until(...)` and then either keep waiting later or call `abandon()`.

`transport_failed` should not be introduced as a distinct state unless Sintra
first grows a real internal classification for it. In the current code, many
transport-adjacent failures still surface either as `rpc_cancelled` or as a
serialized remote `std::runtime_error`.

## Meaning of `abandon()`

`abandon()` should mean:

- the caller is no longer interested in the result
- local waiting state may be released
- the handle transitions to a terminal abandoned state exactly once
- late replies must be ignored or dropped safely

It should not mean:

- cancel remote execution
- force a remote exception
- terminate a process

That separation is critical.

Additional required rules:

- `abandon()` must be idempotent
- destroying a still-pending handle must be equivalent to non-blocking
  `abandon()`
- `get()` on an abandoned handle must not report false success
- once a handle reaches a terminal state, later replies or duplicate cleanup
  attempts must become no-ops

## Late replies

Any design in this area must define what happens when the reply arrives after
the caller has already abandoned the request.

Required properties:

- no deadlocks
- no dangling return handlers
- no accidental delivery into unrelated state
- no false success to an abandoned caller

A clean model is:

- a copied in-flight reply callback must re-check the shared RPC state under the
  same mutex before completing the call
- `abandon()` may safely deactivate the current return handler once the shared
  state is already terminal
- if the handle has already been abandoned or cancelled, any later reply must be
  ignored or dropped without changing the terminal state
- deferral remapping must update the current handler id under the same shared
  state lock used by abandonment and completion
- metrics or tracing may record that the reply arrived late

This matters because the reply reader may already have copied a handler before
the caller deactivates it. Safe behavior therefore depends on exact-once shared
state transitions, not only on handler-map removal.

Additional invariant:

- deferral messages and the eventual final reply for a given RPC must remain
  serialized through the same reply-reader execution stream

If that invariant is not preserved, handler-id remapping can race the final
reply dispatch and lose the completion entirely.

## Same-process targets

This proposal must define how local same-process targets behave.

Today, non-strict synchronous RPC can bypass the transported path and call the
target directly. That is compatible with a synchronous API, but it is not by
itself an honest async/deadline seam.

If a public async handle is added, the design should choose one explicit rule:

- either handle-based RPC always uses the transported request/reply path, even
  for same-process targets
- or local same-process handle-based RPC is out of scope initially

What should be avoided is an API that appears deadline-aware while silently
executing some local calls synchronously inline.

For an initial implementation, explicitly rejecting same-process non-strict
targets is a valid narrower choice if preserving current reentrancy semantics
matters more than maximizing coverage on day one.

## Finalize and teardown interaction

Handle lifetime during teardown must be explicit.

Required properties:

- outstanding handles must reach a deterministic terminal state before the local
  `Managed_process` infrastructure is torn down
- handle destruction must not dereference process-owned handler maps after
  finalization has begun
- caller-side abandonment and process teardown must share the same cleanup rules

In practice, that means the implementation should not rely on a live
`Managed_process` still existing after finalization starts. The retained handle
state must be sufficient to report the terminal outcome to the caller.

This proposal also does not automatically solve bounded receiver-side drain in
`ensure_rpc_shutdown()`. Caller-side abandonment helps the waiting side stop
blocking, but a callee that is already waiting for its own active handlers is a
separate teardown problem.

The current implementation intentionally keeps `ensure_rpc_shutdown()` blocking
until active handlers finish and only adds stalled-shutdown warnings. A forced
deadline there would let the transceiver object disappear while one of its RPC
bodies is still executing against it, which is not a safe trade.

## Lock ordering

Any implementation in this area must preserve the existing lock discipline.

Current ordering is:

1. `s_outstanding_rpcs_mutex`
2. per-RPC state mutex
3. return-handler-map mutex

Reply dispatch must continue to copy handler state while holding the
return-handler lock and then release that lock before invoking callbacks.
Abandonment, cancellation, deferral remapping, and terminal completion all need
to respect that same ordering.

## Process liveness vs RPC lifecycle

These are separate concepts and should remain separate.

### RPC lifecycle tells us

- whether the caller is still waiting
- whether a reply arrived
- whether the wait was cancelled
- whether the caller abandoned interest

### Process lifecycle tells us

- whether the remote process is still alive
- whether the remote endpoint is draining
- whether escalation to process termination is required

The proposal becomes most valuable when higher-level systems stop conflating the
two.

For example:

- a caller-side deadline expiry should not imply process kill
- a process exit should complete or fail outstanding RPCs deterministically

## Where this can help downstream systems

This is not only relevant to one framework or one application.

Potential benefits include:

- cleaner stop/unload/shutdown orchestration in worker-host frameworks
- better GUI shutdown flows that do not need ad hoc watchdog threads
- more honest lifecycle handling for long-running RPCs
- easier recovery logic when a caller wants bounded waiting but the callee may
  still be doing real work
- fewer duplicated timeout patterns in applications built on Sintra

## Why this might still belong outside Sintra

There is also a valid argument for not changing Sintra yet.

That argument is:

- Sintra already exposes the necessary low-level behavior
- the current pain may come more from application/session shutdown structure
  than from missing IPC primitives
- any public async RPC API would increase surface area and long-term
  maintenance cost

This is a serious concern. A larger API should not be added merely because one
downstream stack used watchdogs badly.

## Criteria for deciding whether this belongs in Sintra

A Sintra-level change would be justified if:

1. the abstraction is truly generic
2. it maps naturally to existing Sintra internals
3. it removes repeated higher-level reinvention
4. it does not misrepresent cancellation semantics
5. it can be introduced without destabilizing the synchronous API

If those conditions are not met, the better answer may be:

- keep Sintra as-is
- improve framework/app shutdown ownership instead

## Recommended scope if pursued

If this is pursued, the recommended scope is deliberately narrow:

### Stage 1

Expose a low-level async RPC handle internally or experimentally:

- create request
- wait
- try-wait
- abandon
- inspect terminal state

No deadlines yet, except as a thin `wait_until(...)` convenience.

### Stage 2

Add a public async handle surface:

- `rpc_async_<method>(...)`

The public handle can already support bounded waiting through
`wait_until(...)`, so a separate `rpc_with_deadline(...)` wrapper is not
needed in the initial scope.

This does not change the public meaning of synchronous `rpc_<method>(...)`.
That remains a blocking call/return surface. The transported synchronous path
may reuse the same internal shared state as `Rpc_handle`, but callers should
still treat sync RPC and async-handle RPC as distinct usage models rather than
as interchangeable styles of the same public API.

### Explicitly defer

- a separate `rpc_with_deadline(...)` convenience wrapper
- tracing or counter-oriented diagnostics
- remote cooperative cancellation protocol
- preemptive handler interruption
- automatic process termination on deadline expiry

## Potential pitfalls

This design would need careful review for:

- exact-once terminal-state transitions
- return-handler lifetime after abandonment
- deferral path correctness
- outstanding RPC set cleanup
- interaction with process shutdown and coordinator teardown
- lock ordering and notification ordering
- behavior when a reply races with abandonment

These are implementation-sensitive details and are exactly why this should be
introduced cautiously, if at all.

## Summary

Sintra already contains most of the machinery needed for caller-side RPC
abandonment:

- per-RPC wait state
- outstanding RPC tracking
- process-scoped unblock on some shutdown paths
- receiver-side RPC drain control

What is still missing is the precise lifecycle contract:

- explicit terminal states
- exact abandonment semantics
- local-target rules
- teardown-safe cleanup rules

What it does not currently expose is a clean public model for:

- asynchronous RPC waiting
- deadline-bounded waiting
- explicit abandonment of caller interest

That is the narrow, promising improvement area.

The right interpretation is:

- caller-side deadline handling is a good fit for Sintra
- true remote cancellation is not
- process kill remains the hard escalation boundary

If this idea is pursued, it should be reviewed as a careful lifecycle API
addition, not as a broad redefinition of Sintra RPC semantics.

One practical acceptance criterion is that Sintra should be able to replace its
own ad hoc finalize-time watchdog around `rpc_begin_process_draining(...)` with
this primitive before claiming the surface is ready.
