# Footgun Review and Fail-Fast Plan

This document defines the purpose and scope of the current review effort.
It exists to keep the work focused on eliminating misuse-prone contracts and
late-failing behavior, rather than drifting into a sequence of unrelated
point fixes.

## Problem statement

Several recent failures were not isolated implementation bugs. They exposed a
broader class of footguns:

- APIs and helpers whose contracts are easy to misunderstand
- synchronization patterns that appear to work until a slow or stressed host
  exposes the misuse
- teardown and lifecycle assumptions that fail late, indirectly, or only on
  certain platforms
- tests and fixtures that accidentally encode risky patterns instead of
  flagging them

This is especially dangerous because a real application may carry such misuse
for a long time before it becomes visible.

## Primary goal

Identify misuse-prone contracts across the runtime, helper layer, and tests,
then address them in one of two ways:

1. Prevent the misuse structurally when possible.
2. When prevention is not practical, detect the misuse early and fail loudly.

The preferred outcome is not "fewer flaky tests." The preferred outcome is:

- fewer invalid patterns are expressible
- remaining invalid patterns trip explicit diagnostics
- misuse fails near the point where it is introduced, not much later during
  shutdown, stress, or platform-specific timing

## Non-goals

This effort is not primarily about:

- polishing documentation in isolation
- making existing footguns merely easier to remember
- chasing only whichever test happened to fail most recently
- preserving old helper behavior just because some test or call site depends
  on it

Broad source breakage during migration is expected and intended if that is
what it takes to remove ambiguous lifecycle contracts. A large number of
affected call sites is not, by itself, a reason to keep a misleading public
API shape in place.

## Review questions

Every investigation in this workstream should try to answer these questions.

### 1. What is the actual contract?

For each helper or primitive, determine the real contract rather than the
likely user interpretation.

Examples:

- What does a barrier actually guarantee?
- What does `shutdown()` require from all participants?
- What is safe to do after a final `_sintra_all_processes` rendezvous?
- What does a processing fence guarantee that a plain rendezvous does not?

### 2. How is the contract likely to be misread?

For each surface, identify the plausible mistake an experienced caller might
still make.

Examples:

- using a rendezvous as if it were a processing drain
- layering a custom final barrier on top of `shutdown()`
- assuming a post-barrier file write is already visible to the root
- hiding coordinator/root participation inside a helper so barrier cardinality
  is split across multiple locations
- relying on `sleep_for(...)` instead of synchronization

### 3. Can the misuse be made impossible?

Prefer structural fixes over comments.

Examples:

- remove or narrow misleading helpers
- replace ambiguous helpers with ones that encode the intended protocol
- separate APIs for distinct lifecycle patterns instead of overloading one
  helper with multiple interpretations

### 4. If misuse cannot be prevented, can it fail immediately?

Where invalid patterns remain possible, add checks that turn a latent stress
failure into an immediate failure or explicit diagnostic.

Examples:

- assertions in helpers when a forbidden combination is detected
- debug-mode contract checks around shutdown participation
- validation that required phases completed before teardown proceeds
- stronger diagnostics when barriers or RPC waits are cancelled during a phase
  that should have completed normally
- rejection of "safe" APIs that still require callers to provide barrier names,
  group names, intermediate phase labels, or protocol-object bookkeeping

### 5. Do the tests prove the safety mechanism?

Every safety rule should have a test shape that proves one of these:

- the safe pattern succeeds
- the unsafe pattern is rejected immediately
- the unsafe pattern produces a clear, deterministic diagnostic

## Investigation scope

This review is broader than shutdown, but shutdown is an important initial
focus because it exposed several of the latent problems.

### A. Shutdown and teardown protocols

Review:

- `sintra::shutdown()`
- raw teardown primitives, including `detail::finalize()`
- helper layers that wrap them
- tests that use custom final barriers or post-barrier work

Questions:

- Is the standard lifecycle story clearly centered on `shutdown()` rather than
  split between `shutdown()` and `finalize()`?
- Is the required participation pattern explicit?
- Can a caller layer a second teardown rendezvous on top of shutdown?
- Can important side effects still happen after the supposed final handoff?
- Are cancellation paths surfacing too late?

Current direction:

- the standard safe path is `shutdown()` or `shutdown(shutdown_options{...})`
- direct `detail::finalize()` and manual final barrier choreography are
  low-level escape hatches, not peer alternatives in the normal lifecycle
  story

### B. Barrier semantics

Review all places that use:

- plain rendezvous barriers
- `processing_fence_t`
- all-process final barriers

Questions:

- Is a rendezvous being treated as a processing guarantee?
- Is a barrier being used as a "safe to finalize now" handshake?
- Is the barrier membership obvious at the call site?
- Is root/coordinator participation visible or hidden in a harness?

### C. Timing-based assumptions

Review code that uses:

- `sleep_for(...)`
- ad hoc polling
- "small delay to ensure..." comments

Questions:

- Is the timing masking a missing synchronization primitive?
- Can this be replaced with an explicit fence, barrier, or handshake?
- If not, can the test or runtime assert that the expected state actually
  occurred before proceeding?

### D. Helper and harness surfaces

Review:

- test helpers
- convenience wrappers
- lifecycle helpers
- any API that hides participation or ordering

Questions:

- Does the name suggest a stronger guarantee than the implementation provides?
- Is the critical protocol visible in one place, or split across helper and
  caller?
- Can the helper be narrowed or split so invalid combinations are harder to
  express?
- Does the supposedly safe API still make the caller name intermediate phases
  or manage protocol objects manually?

### E. Messaging and RPC lifecycle assumptions

Review interactions between:

- barrier completion
- message delivery
- RPC completion and cancellation
- unpublish and teardown

Questions:

- Are we assuming delivery is complete when only rendezvous is complete?
- Can a late RPC cancellation be mistaken for a test failure caused earlier?
- Are diagnostics strong enough to distinguish misuse from normal teardown?

## Expected outputs

This effort should produce more than individual fixes.

At minimum, the output should include:

1. A categorized inventory of footgun classes.
2. For each class:
   - the actual contract
   - the likely misuse
   - whether it can be prevented structurally
   - what fail-fast mechanism should exist if prevention is not possible
3. A set of recommended API/helper changes.
4. A set of recommended debug/runtime assertions or diagnostics.
5. Targeted tests that prove misuse is caught early.

The recommended API output should prefer:

- one clear default surface for the common case
- optional intent-bearing arguments over families of subtly different helper
  names
- established Sintra terminology such as `coordinator` instead of ambiguous
  words such as `root`
- caller-facing names that do not pull low-level teardown primitives such as
  `finalize` back into the public vocabulary
- supported shutdown variants expressed through `shutdown_options` by default,
  rather than a second family of loosely related helper names
- separate public shutdown helpers only when the operation is no longer
  semantically "shutdown with declared extra behavior"
- implementation prompts and reviews that reject the conservative fallback of
  "keep the old public API and add checks around it" unless a concrete blocker
  is identified and explained

## Decision rule

When a concrete issue surfaces, do not stop at "how do we fix this test?"

Instead ask:

- Which footgun class does this belong to?
- Is the current fix removing the class, or only repairing one instance?
- If it is only repairing one instance, what guardrail is still missing?

If a proposed change does not improve one of:

- prevention
- immediate detection
- diagnostic clarity

then it is probably not addressing the real problem.

## Working principle

The direction of this workstream is:

- make valid lifecycle and synchronization patterns explicit
- make invalid patterns hard to write
- make remaining invalid patterns fail early

Tests and experiments are not user-facing API consumers. They may use low-level
or `detail` surfaces when that is necessary to exercise primitives directly.
That should not be used as justification for keeping low-level teardown
primitives in the main public namespace.

Supported non-happy-path application scenarios should still receive explicit
public lifecycle support. If a shutdown shape is realistic and intended for
user code, it belongs in `shutdown_options` unless it is no longer
semantically just shutdown. Raw teardown primitives belong in `detail` for
tests, experiments, and deliberate low-level work.

Implementation work should therefore assume the following by default:

- breaking tests, examples, and ordinary call sites during migration is
  acceptable
- updating those call sites to the cleaner lifecycle surface is part of the
  intended work
- preserving a misleading public `finalize()`-shaped user path just to avoid
  migration churn is not an acceptable default
- if an implementation pass keeps that path, it must name the concrete
  technical blocker rather than appealing only to breadth of breakage

That should remain the primary goal even when specific CI failures or test
regressions pull attention toward a narrow local fix.
