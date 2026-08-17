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
- Reported hardware faults on Sintra-owned Windows threads. A fault on a ring
  reader thread, where application message handlers run, reaches no CRT signal
  path; it now runs the abnormal-termination dispatch instead of passing unseen,
  so a faulted peer is retired and barriers stop waiting for a participant that
  no longer exists.
- Preserved crash provenance on Windows. A faulted process now exits with the OS
  exception code and names the cause on stderr, instead of exiting with a generic
  `1`, and the host's crash reporter and Windows Error Reporting still run.
- Gave the host the final say on both Windows fault paths. Sintra consults
  `UnhandledExceptionFilter` before declaring a death, so a fault the host repairs
  no longer broadcasts `terminated_abnormally` or stops the ring readers, and a
  crash watchdog still guarantees that a faulted peer cannot linger.

### Compatibility

- Restored custody-relative recovery numbering: every fresh managed-child
  custody, including process-id reuse and mid-flight joins, starts at occurrence
  `0`; its first recovery is `1`.
- Exact managed-child exit identities now include an opaque runtime-scoped
  custody identity so separate custodies remain distinct without redefining
  recovery occurrence semantics.
- Bumped the ring ABI to version 8 for the internal joined-process startup
  protocol. All processes in a swarm must use binaries built against the same
  Sintra ring ABI.
- Windows fatal-signal exit statuses changed. A hardware fault now exits with the
  OS exception code, for example `0xC0000005`, instead of `1`, and `SIGABRT` and
  `SIGTRAP` exit with `3`. `SIGINT` and `SIGTERM` still exit with `1`. Code that
  compared a managed child's Windows exit status against `1` must test for a
  non-zero status instead; the exact code is not a contract.
- `SINTRA_HAS_SEH` selects the Windows per-thread fault guard. It defaults to `1`
  for MSVC and clang-cl and `0` elsewhere; clang targeting `*-w64-windows-gnu`
  must opt in with `-DSINTRA_HAS_SEH=1 -fms-extensions`, and MinGW GCC builds keep
  the previous behaviour because GCC does not implement `__try`/`__except`. Define
  it identically for every translation unit that includes Sintra.
- `SINTRA_CRASH_WATCHDOG_GRACE_MS` (default 5000) bounds how long a host crash
  reporter may run on a faulted Windows process before Sintra ends it.

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
