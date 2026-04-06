# Protocol-Shape Guardrails And Architecture Direction

This document revises the earlier direction again.

The previous revision correctly pushed toward structural prevention, but it
still had a major weakness:

- the proposed safe surface still looked too much like a dressed-up version of
  the low-level barrier API

That is not good enough.

If the caller still has to think in terms of:

- barrier names
- group names
- intermediate phase labels
- token consumption
- or special protocol objects that must be remembered and consumed

then the API is still too close to the machinery and too far from the user's
intent.

The purpose of this document is to make the target direction stricter and
cleaner:

- keep the common case simple
- keep the public surface predictable
- remove ambiguous words
- and let the runtime own the protocol choreography

## Self-review of the previous revision

The previous revision improved the direction in several ways:

- it moved from "review better" toward "prevent structurally"
- it proposed removing misleading harness surfaces
- it proposed fail-fast enforcement for illegal composition

But after review, it still had four meaningful flaws.

### 1. It still leaked low-level assembly details

The earlier idea of:

```cpp
auto done = sintra::collective_done("work-done", "_sintra_all_processes");
done.finalize_all();
```

is better than open-coding the whole protocol, but it still makes the caller
provide:

- a barrier label
- a group name
- an intermediate protocol step

That is still barrier-shaped thinking, not protocol-shaped thinking.

### 2. It risked replacing one footgun with another

The earlier "consumption semantics" idea tried to make protocol correctness
depend on whether a C++ object was consumed exactly once.

That is dangerous in its own right:

- temporaries
- moves
- helper forwarding
- ignored values
- destructor-time failures

The runtime should enforce protocol rules through explicit runtime protocol
state, not by making correctness depend on object-lifetime rituals.

### 3. It introduced too many new names

The earlier draft leaned toward multiple new top-level helper names and
multiple new harness entry points.

That risks API sprawl:

- many similar names
- duplicated concepts between runtime and tests
- more vocabulary for callers to learn

### 4. It still used overly mechanical names

Some of the suggested names were still wrong because they described internal
mechanics rather than caller intent.

Examples that should not shape the API:

- anything with `publish`
- anything with `root`
- anything with `broadcast`
- anything with `phase`

Those are either overloaded in Sintra already or too internal to be a clean
public surface.

## Revised objective

The objective is now:

1. keep `shutdown()` as the simple, standard terminal API
2. extend `shutdown(...)` with optional, intent-bearing arguments for the
   non-trivial case
3. avoid exposing barrier names, group names, or intermediate protocol labels
   in the standard safe path
4. enforce illegal combinations using runtime protocol state, not token
   consumption
5. reserve direct low-level final choreography for `detail` and explicit
   primitive-level work

This is a simpler public direction than the previous revision.

## Core design decision

The cleanest surface is not:

- many `shutdown_after_*` functions
- many `run_multi_process_*` helper names
- or a token-producing pre-step such as `collective_done(...)`

The cleanest surface is:

- one main terminal verb: `shutdown`
- one simple default: `shutdown()`
- optional arguments that express what must happen around shutdown

That keeps the caller mental model small:

- normal case: `shutdown()`
- special case: `shutdown(...)` with one clearly named option

## Keep the simple case simple

The current `shutdown()` shape is still the best default for the narrow,
symmetric case:

- all participants reach the same top-level handoff
- no important final side effect remains
- teardown may begin immediately

That should remain the standard path.

The redesign should not make this case more ceremonial.

Just as importantly, the documentation should make this hierarchy explicit:

- ordinary callers use `shutdown()` or `shutdown(shutdown_options{...})`
- supported non-happy-path shutdowns should also be expressed through
  `shutdown_options` unless they are no longer semantically just shutdown
- raw teardown primitives and direct final barrier choreography live in
  low-level `detail` territory

In other words, `shutdown()` should own the standard terminal path.

## Do not expose protocol mechanics in the API

The standard safe path should not require callers to spell:

- `_sintra_all_processes`
- barrier names
- "done" labels
- "result-ready" labels
- internal acknowledgment names

Those are implementation mechanics.

The library should own them internally.

If a caller needs to express an unusual custom protocol directly, that should
be treated as an explicit low-level escape hatch in `detail`, not the normal
API.

## Better public direction

The public API should stay centered on `shutdown`.

The standard shapes should look like:

```cpp
sintra::shutdown();
```

and:

```cpp
sintra::shutdown(
    sintra::shutdown_options{
        .coordinator_shutdown_hook = [&] {
            write_summary_file(path);
        }
    });
```

This is not a final API spelling, but it illustrates the right shape:

- `shutdown` stays the main operation
- the caller does not name barriers or phases
- `coordinator` uses existing Sintra terminology
- `shutdown_hook` describes the role of the callback without re-exposing
  `finalize` as part of the public vocabulary
- the callback body itself says what domain-specific action happens

That is cleaner than:

- `shutdown_after_*`
- `collective_done(...)`
- `root_action_then_finalize(...)`
- or anything involving `publish`, `broadcast`, or `phase`

## Why `coordinator_shutdown_hook` is better than earlier names

It is not perfect, but it is much better because:

- `coordinator` is an established Sintra term
- `shutdown_hook` says clearly that this is a callback slot within shutdown
- the callback body describes the actual work

That is a major improvement over names such as:

- `root_action`
- `result_write`
- `end_phase`
- `root_decide_then_broadcast_and_shutdown`

Those names force the reader to guess too much.

## Verification should usually stay outside shutdown

One of the most important corrections from review is that "verification" is
usually not a protocol phase at all.

If only the coordinator cares about the final check, the natural shape is:

```cpp
sintra::shutdown();
const bool ok = validate_reports(shared_dir);
```

This is better than embedding verification into the shutdown API because:

- it avoids blocking peers on a root-local concern
- it avoids inventing ambiguous names such as `root_verify_then_finalize`
- it keeps shutdown focused on coordination and lifecycle, not post-mortem
  local checks

Only if peers must depend on some coordinator-side final decision before
teardown should there be a second API shape, and that should be motivated by
an actual use case rather than introduced speculatively.

## Recommended API scope

The initial supported public shapes should be minimal.

### Shape 1: simple collective shutdown

```cpp
sintra::shutdown();
```

Semantics:

- collective symmetric handoff
- shutdown owns the standard terminal path
- the library performs the collective handoff and proceeds to finalization
- ordinary callers should not pair this with extra final all-process barriers
  or a direct subsequent `detail::finalize()`

Guarantees:

- all live participants that use the standard path have entered the same
  standard shutdown protocol
- the standard shutdown choreography, including any internal synchronization
  required by shutdown, is owned by the runtime

Does not guarantee:

- that arbitrary caller-defined post-shutdown side effects have happened
- that any coordinator-local verification has run
- that low-level custom barrier choreography has been integrated into the
  standard path

### Shape 2: coordinator-side shutdown hook

```cpp
sintra::shutdown(
    sintra::shutdown_options{
        .coordinator_shutdown_hook = [&] {
            write_summary_file(path);
        }
    });
```

Semantics:

- all participants call `shutdown(...)`
- if a coordinator shutdown hook is configured, non-coordinator participants
  wait inside the standard shutdown path while the coordinator runs one
  bounded coordinator-local callback
- the library owns the necessary synchronization
- peers do not need to know any internal barrier names or internal shutdown
  stages

Guarantees:

- the coordinator hook runs at a defined point within shutdown before raw
  teardown begins
- non-coordinator participants remain inside the standard shutdown path while
  that hook is active
- the runtime owns any internal synchronization needed before raw teardown
- the hook is coordinator-local and must not initiate new peer coordination,
  extra barriers, or additional custom protocol steps

Does not guarantee:

- that the hook is appropriate for arbitrary long-running work
- that partially completed side effects can be rolled back automatically
- that peers remain available for new caller-defined coordination while the
  hook is running

Failure semantics that must be defined explicitly in the eventual API:

- if the coordinator hook throws, shutdown fails and that failure is surfaced
  explicitly
- if the coordinator hook blocks indefinitely, peers must not silently drift
  into unrelated teardown paths; this must end in a bounded, attributable
  failure
- if the coordinator hook performs a partial side effect before failing, that
  is a caller-level concern; the runtime may coordinate state, but it cannot
  infer semantic rollback

That is enough to cover the failure classes we have already seen:

- post-collective final file or summary write
- symmetric shutdown with one designated coordinator-side last step

Do not introduce more shapes until a concrete case justifies them.

The default extension rule should be:

- if the scenario is still semantically "shutdown, with declared extra
  behavior", it stays inside `shutdown_options`
- introduce a separate public helper only if the operation is no longer
  semantically just shutdown

If realistic application error-path or degraded-shutdown cases emerge, they
should follow that rule. They should not force ordinary callers onto raw
teardown primitives.

## Runtime enforcement should use protocol state, not token state

The runtime should track active shutdown protocol state explicitly.

It should not infer correctness from whether a helper-produced object was
consumed.

### Recommended state model

Conceptually:

- idle
- collective_shutdown_entered
- coordinator_hook_running
- coordinator_hook_completed
- finalizing

The exact state names can differ, but the important point is:

- the runtime knows whether a standard shutdown protocol is active
- `detail::finalize()` can reject illegal transitions directly
- a second conflicting final handoff can be rejected directly

This is much cleaner than tying correctness to moves and destructors.

## Harness direction

The test harness should not invent a parallel family of lifecycle names.

Instead, it should use the same end-protocol vocabulary as the runtime.

The intended direction is:

- harness APIs pass through `shutdown_options`
- harness APIs do not invent a separate lifecycle vocabulary

What should be avoided is:

- one set of names in runtime
- another set in tests
- hidden coordinator participation in helpers
- split protocol membership across helper and process bodies

## Low-level escape hatch

There must still be a way to write custom explicit final protocols for:

- primitive barrier tests
- pathology tests
- deliberately unusual workflows

But that path should be visibly low-level and outside the main public
lifecycle surface.

The standard path should not look like the low-level path.

This can be achieved by:

- keeping raw barriers and raw teardown primitives under `sintra::detail`
- stating explicitly that ordinary callers are not expected to use
  `detail::finalize()` or manual final `_sintra_all_processes` choreography
- linting or flagging direct final `_sintra_all_processes` choreography in
  normal tests
- and documenting that such code is a primitive-level path, not the default
  one

## Fail-fast strategy

The enforcement strategy should now be:

### 1. Keep the common API tiny

Fewer public terminal shapes means fewer ways to misunderstand them.

### 2. Let the API express intent, not choreography

The caller should say:

- plain shutdown
- shutdown with a coordinator-side final hook

and the runtime should own the choreography.

### 3. Reject illegal composition using runtime state

Examples:

- `detail::finalize()` called while a standard shutdown protocol is active but
  not in the right state
- `detail::finalize()` called after the standard `shutdown(...)` path has
  already been selected
- direct final all-process barrier use while a standard shutdown protocol is
  active
- mixing a coordinator hook protocol with extra manual final handoffs

### 4. Lint the obvious test misuse shapes

In tests, direct final `_sintra_all_processes` choreography should be
disallowed except in explicit primitive or pathology cases.

That catches:

- final barrier immediately followed by `detail::finalize()`
- helper-managed shutdown combined with another final collective barrier
- barrier membership split between helper and process bodies

### 5. Keep diagnostics lifecycle-oriented

Diagnostics should mention:

- collective shutdown entry
- coordinator final hook
- coordinator hook completion
- raw teardown transition

not internal, caller-visible "phase" terminology.

## What should be removed from the previous proposal

The next design iteration should explicitly remove these ideas unless a later
use case proves them necessary:

- `collective_done(...)` as the public entry point
- token-consumption enforcement
- `root_verify_then_finalize(...)`
- multiple `shutdown_after_*` function names
- user-supplied barrier names in the standard safe path

## Concrete next-step recommendation

The next design revision should be centered on this question:

Can the entire standard terminal coordination surface be expressed as:

- `shutdown()`
- `shutdown(shutdown_options{...})`

If the answer is yes, that is probably the better API direction.

If the answer is no, the justification should name a concrete protocol shape
that cannot be expressed cleanly that way.

The intended default answer should be yes.

## Decision rule

Any proposed safe API should fail this review if it still requires ordinary
callers to:

- name intermediate phases
- supply barrier names
- supply `_sintra_all_processes`
- manage protocol objects manually
- or interpret internal choreography terms

If the caller still has to do any of those in the standard path, the API is
still too low-level.

## Short normative summary

The standard lifecycle story should be:

- `shutdown()` for the ordinary collective case
- `shutdown(shutdown_options{...})` when the coordinator must run one defined
  shutdown hook before raw teardown
- additional public shutdown helpers only when a supported case is no longer
  semantically just shutdown and cannot be expressed cleanly through
  `shutdown_options`

Everything else:

- direct `detail::finalize()`
- direct final `_sintra_all_processes` choreography
- mixed manual shutdown protocols

should be treated as low-level protocol work for tests, experiments, and
deliberate primitive use, not as part of the standard safe interface.
