# Deferred development and current limitations

## Status and owner decision

On 2026-09-29 the owner chose to use the current Sintra for a product release
and defer the developments below. This records that scope decision; it does
not certify a particular application's release readiness. No implementation
work on these tracks is scheduled or authorized by this document.

This inventory was checked against `9389e8fd`. Current API contracts remain in
the [reference](reference/index.md), [architecture](architecture.md), and
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

External design revision 7 still had review blockers. Its issues included
occurrence identity/binding, outcome retirement and ordering, shutdown
progress, and attribution of barrier loss facts. Its mechanics must be
reconciled with subsequent code changes before reuse. No particular journal,
marker protocol or API shape is adopted here.

### Owning values for variable-field `receive<T>()`

**State: current limitation; ownership repair deferred.** Ordinary decoded
values such as `receive<std::string>()` and complete fixed-size values are
distinct from generated `SINTRA_MESSAGE` objects containing `message_string`
or other `typed_variable_buffer` fields. Those fields are self-relative wire
descriptors, not independent payload owners. `receive<T>()` copies/moves the
message object through its callback, optional storage and return value;
it does not relocate or own the trailing payload. Accessing such a returned
descriptor is unsafe even before the next message is processed. Copying its
fields after return is not a repair.

Current applications should receive ordinary owning value types where
applicable, or decode variable fields from a valid const-reference message
callback into application-owned values before the callback returns. The
[receive reference](reference/receive.md) records this boundary.

The deferred requirement is independent value lifetime across return,
copy/move and later reads, while preserving exactly one copy out of the ring.
A mandatory wrapper, changed return type, blanket second frame copy or new
wire representation has not been adopted. This ownership work was explicitly
separated from the eviction protocol.

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
filesystem requirements remain in the [README](../README.md#local-account-and-ipc-resources).

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

## Historical observations to revalidate if reopened

These are preserved investigation leads, not newly reproduced failures of
`9389e8fd` or a current failure-rate claim:

- The 2026-09-28 handoff reports one local
  `complex_choreography_stress_test` broadcast/delivery timeout, followed by
  120/120 isolated passes; its cause was unknown.
- The same handoff records a concern that a returning application signal
  handler can leave a live process reported as terminated because Sintra
  dispatches termination handling before chaining to the application handler.
  Recheck current signal handling and define the returning-handler contract
  before selecting a fix. This is separate from the landed preservation of
  `SIG_IGN` and the documented lifecycle-worker shared-fate behavior.

## Rejected proposal

Replacing the processing barrier's second meeting with markers and
acknowledgements was rejected on 2026-09-27. It is not deferred implementation
work. The reasons remain under **Considered and rejected** in the
[design principles](design_principles.md).

## Evidence and historical design provenance

Current source anchors at `9389e8fd`:

- `include/sintra/detail/runtime.h`: `receive`.
- `include/sintra/detail/messaging/message.h`: `variable_buffer`,
  `typed_variable_buffer` (self-relative offsets and default copying).
- `include/sintra/detail/ipc/process_utils.h`: `cached_process_instance`,
  `current_process_instance`, `process_instance_has_exited`.
- `include/sintra/detail/ipc/spinlock.h`: owner-word recovery and stall checks.
- `include/sintra/detail/transceiver_impl.h`: destructor and `destroy`.

Historical investigation files were outside this repository under
`C:\plms\varinomics\_agent_reports\`:

- `sintra_eviction_20260927/DESIGN.md` (revision 7), `CONTRACT.md`,
  `review_r7_codex_astra.md`, `review_r7_claude_opus.md`.
- `sintra_remediation_20260926/receive_ownership_design.md` and
  `residual_contract_triage.md` (SM-05 and SM-06).
- `HANDOFF_sintra_20260928_final.md`, section 7 (historical observations).

Those files provide design history, not current API authority. This in-repo
inventory preserves the deferred scope and limitations without requiring
access to that local report directory.
