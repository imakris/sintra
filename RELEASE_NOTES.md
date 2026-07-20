# Release Notes

## Unreleased

### Highlights

- Added exact managed-child exit observation with immutable occurrence
  identity, late exactly-once delivery, quiescent cancellation, normalized and
  native status, and lifecycle-thread exception containment.
- Scoped recovery authority and retained launch recipes to managed-child
  custody, preventing opt-in or delayed runners from crossing process-id reuse.
- Hardened managed and external publication retirement with exact reader
  generation checks and ordered publication notifications.
- Added exact managed-child detach with explicit disowned custody status,
  detached external invitations, native event-driven coordinator observation,
  and member-side lifecycle callbacks that survive coordinator departure.
- Detached generations are notified and excluded before collective-shutdown
  lifecycle barriers, while existing managed children and external invitations
  remain coordinator-bound by default.

### Compatibility

- Restored custody-relative recovery numbering: every fresh managed-child
  custody, including process-id reuse and mid-flight joins, starts at occurrence
  `0`; its first recovery is `1`.
- Exact managed-child exit identities now include an opaque runtime-scoped
  custody identity so separate custodies remain distinct without redefining
  recovery occurrence semantics.
- Bumped the ring ABI to version 9 for the joined-process startup protocol,
  detached external-invitation claim metadata, and collective departure
  notice. All processes in a swarm must use binaries built against the same
  Sintra ring ABI.

## v1.2.0 (2026-04-28)

### Highlights
- Added install/export support for CMake consumers, including the
  `sintra::sintra` namespaced target and generated package config files.
- Added the static API reference site and expanded symbol-level reference
  documentation.
- Documented the public
  `sintra::disable_debug_pause_for_current_process` helper.
- Added the public `<sintra/rings.h>` facade for direct ring helper usage.
- Added typed `sintra::rpc_unavailable` propagation so unavailable targets
  are distinguishable from other remote runtime errors.
- Documented the process-granular barrier participation rule: each process
  should have at most one in-flight caller for a given barrier round.
- Hardened RPC dispatch/destruction lifetime handling and ring control-block
  attach validation.
- Improved release-build error handling for instance-id exhaustion and
  surfaced console RPC-print failures through the log callback.

### Compatibility
- Consumers can now catch `sintra::rpc_unavailable` directly when an RPC
  target is unavailable.
- `activate_slot`, `deactivate_all_slots`, and `enable_recovery` now report
  calls without an active runtime with `std::runtime_error`.

### Test Infrastructure
- Added targeted RPC, RPC destruction race, typed RPC-unavailable, and ring
  ABI fingerprint regression coverage.
- Expanded CI stress coverage for FreeBSD-sensitive test paths.

## v1.0.3 (2026-02-21)

### Highlights
- Fixed multiple lifecycle and synchronization races in IPC ring and message reader paths.
- Fixed RPC handler lifetime and exception cleanup behavior.
- Improved platform stability in CI/stress scenarios (including Windows and FreeBSD fixes).
- Expanded and hardened test coverage, including defensive and coverage-reporting updates.
- Refactored and deduplicated internal code paths to reduce complexity and improve maintainability.

### Compatibility
- No intended API break for existing consumers.
