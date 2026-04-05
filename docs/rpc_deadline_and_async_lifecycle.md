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

### 1. Outstanding RPC waits can already be unblocked

`Managed_process::unblock_rpc(...)` marks matching outstanding RPCs as
cancelled and wakes their waiters.

Relevant code:

- `include/sintra/detail/process/managed_process_impl.h`
- `include/sintra/detail/transceiver_impl.h`

This is already used during communication teardown and some shutdown paths.

### 2. Synchronous RPC already has a caller-side wait state

`Transceiver::rpc_impl(...)` already materializes a per-call `Rpc_state`,
registers return handlers, waits on a condition variable, and distinguishes:

- normal return
- remote exception
- generic RPC failure
- cancelled wait via `rpc_cancelled`

This is an excellent starting point because the lifecycle state already exists,
even though it is currently exposed only through the synchronous API.

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
already has the right internal seam.

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
auto handle = Some_service::rpc_async(target, arg1, arg2);

switch (handle.wait_until(deadline)) {
case rpc_wait_status::completed:
    auto value = handle.get();
    break;

case rpc_wait_status::deadline_exceeded:
    handle.abandon();
    break;

case rpc_wait_status::cancelled:
    break;

case rpc_wait_status::transport_failed:
    break;
}
```

Possible companion forms:

```cpp
auto result = Some_service::rpc_with_deadline(target, deadline, arg1, arg2);
```

where `result` would explicitly distinguish:

- completed
- remote_exception
- cancelled
- deadline_exceeded
- transport_failed

## Why an async handle is better than a sync timeout wrapper

A synchronous wrapper with timeout can be useful, but it is not enough on its
own. The important capability is not merely "wait for at most N milliseconds".
The important capability is:

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

### Likely internal refactoring

Today, `rpc_impl(...)` constructs `Rpc_state` and then blocks until completion.

An async variant would instead:

1. construct and register `Rpc_state`
2. write the request
3. return a lightweight handle that owns/shared-owns the RPC state
4. let callers wait, poll, or abandon
5. clean up return handlers and outstanding state when the lifecycle completes

The synchronous path could then be defined in terms of the async path:

- create handle
- wait without deadline
- extract result

That would keep semantics centralized.

## Meaning of `abandon()`

`abandon()` should mean:

- the caller is no longer interested in the result
- local waiting state may be released
- late replies must be ignored or consumed safely

It should not mean:

- cancel remote execution
- force a remote exception
- terminate a process

That separation is critical.

## Late replies

Any design in this area must define what happens when the reply arrives after
the caller has already abandoned the request.

Required properties:

- no deadlocks
- no dangling return handlers
- no accidental delivery into unrelated state
- no false success to an abandoned caller

A clean model is:

- the reply handler remains valid until final completion or explicit safe
  deactivation
- if the handle has been abandoned, the handler consumes and drops the reply
- metrics or tracing may record that the reply arrived late

## Process liveness vs RPC lifecycle

These are separate concepts and should remain separate.

### RPC lifecycle tells us

- whether the caller is still waiting
- whether a reply arrived
- whether the wait was cancelled
- whether the transport failed

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

Add a public deadline-aware convenience layer:

- `rpc_async(...)`
- `rpc_with_deadline(...)`

with explicit result states.

### Stage 3

Consider optional diagnostics:

- abandoned request counters
- late reply counters
- tracing hooks

### Explicitly defer

- remote cooperative cancellation protocol
- preemptive handler interruption
- automatic process termination on deadline expiry

## Potential pitfalls

This design would need careful review for:

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
- unblock-on-shutdown cancellation
- receiver-side RPC drain control

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
