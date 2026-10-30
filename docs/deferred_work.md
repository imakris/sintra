# Deferred development and current limitations

## Status and owner decision

On 2026-09-29 the owner chose to use the current Sintra for a product release
and defer the developments below. This records that scope decision; it does
not certify a particular application's release readiness. No implementation
work on these tracks is scheduled or authorized by this document.

Current API contracts are in the [reference](reference/index.md),
[architecture](architecture.md), and
[design principles](design_principles.md). Proposed capabilities below must
not be advertised as shipped guarantees. Resume only when the owner reopens
the work, checking the then-current code and obtaining independent review
before adopting a design.

## Deferred capabilities

### Exact eviction, death and barrier outcomes

**State: requirements recorded; replacement protocol not implemented or
approved for implementation.** The direction in the design principles covers
forced advance of leaf readers and accounting for every skipped message
before its ring space is reused. The intended outcome distinguishes a request
not delivered/not executed from an executed request whose reply was lost.
Only affected RPCs, fences and barriers may fail; messages after the gap must
still be delivered. The rings, star topology, trusted coordinator and exactly
one copy out of the ring remain constraints.

The same development track proposes precise loss facts for delivery and
processing barriers, and a global delivery barrier that waits until every
participant has received everything sent before the barrier. These are
proposed extensions, not the current barrier contract; see
[barriers and shutdown](barriers_and_shutdown.md) for that contract.

Unresolved design questions include occurrence identity/binding, outcome
retirement and ordering, shutdown progress, and attribution of barrier loss
facts. No particular journal, marker protocol, or API shape is adopted here.

### Writer-death delivery and the parked-reader watchdog

**State: owner requirements adopted; complete design and implementation
deferred as a separate effort.** A crashed writer's committed messages are to
be delivered, and wake-up notification is
to survive a poster's death so that the parked-reader watchdog can be
removed. Today delivery of a dead writer's last messages depends on timing.
The [writer-death contract](writer_death_contract.md) retains the owner
requirements, unresolved design work, and acceptance boundaries.

## Identity and process-creation residuals

**State: known limitations; complete identity solution deferred.**

- A recoverer can pause after proving an owner dead, then perform a stale
  owner-word CAS after a live owner repeats the same recycled PID and random
  32-bit token. The generation witness used for stall diagnosis does not
  prevent that recovery ABA. A registry that never reissues a `(PID, token)`
  pair was discussed as a candidate, not selected.
- Separately linked copies of the header implementation in one process
  (for example in separate DLLs) need one consistent process identity. If
  their caches draw different tokens, the same-PID/different-token recovery
  rule can mistake a live hold for a dead incarnation. A process-wide identity
  solution and the supported multiple-copy boundary remain unresolved; this
  inventory does not claim a runtime reproduction of that arrangement.
- Ordinary fork resets the instance cache; immediate children bypassing
  atfork redraw when the cached PID differs from their actual PID. This does
  not cover a descendant that never used Sintra and later receives its cached
  ancestor's PID. Shared-address-space clone and reusing an inherited active
  Sintra runtime are not made supported by that redraw.
- Raw fork bypassing atfork can retain a directory lease and delay stale
  cleanup until the inherited descriptor closes or the child execs/exits.
  This is conservative retention, separate from lock recovery identity.

The deployment visibility requirements, foreign-owner UNKNOWN diagnostic,
FreeBSD start-stamp limitations, and generation-wrap assumption remain in the
[design principles](design_principles.md). The same-account and private
filesystem requirements are in the [initialization reference](reference/init.md#ipc-resource-privacy).

## Derived-object teardown

**State: application lifetime obligation today; automatic enforcement deferred.**
Callbacks/exported methods must finish before the derived state they use is
destroyed. A base-class destructor runs after derived members have already
been destroyed, so its cleanup alone cannot enforce that boundary. Arrange
quiescence while that state and the runtime are still alive, using
`destroy()` before member teardown where applicable; do not destroy an object
from its own active exported call. See the
[transceiver lifecycle contract](reference/transceiver.md).

An automatic ownership wrapper, early-destruction diagnostic, and associated
failure policy were investigation candidates, not adopted capabilities.

## Unresolved investigation questions

A broadcast/delivery timeout in `complex_choreography_stress_test` has no
established cause or current reproduction. Treat it as an investigation lead,
not a claimed current failure rate.

The returning-application-signal-handler contract also needs resolution:
termination handling before chaining to the application handler may report a
still-live process as terminated. Recheck the current signal path before
selecting a fix. This question is distinct from preserving `SIG_IGN` and from
the lifecycle worker's documented shared-fate behavior.
