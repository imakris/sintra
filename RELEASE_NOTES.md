# Release Notes

## v2.0.0

### Highlights

- Added managed-child custody with bounded readiness and teardown operations
  and exact exit observation. Exit notifications identify the child occurrence
  and report normalized and available native status, including for late
  subscribers. Recovery opt-in and launch settings stay with that custody
  across restarts and PID reuse. Each fresh custody starts at occurrence `0`
  and its first recovery is `1`.
  [Managed-child API](https://github.com/imakris/sintra/blob/v2.0.0/docs/reference/spawn_swarm_process.md)
- Fixed a coordinator deadlock during process unpublication. Sintra now
  retires only the matching reader generation and cancels pending transported
  RPCs when their reply route closes.
- Ring spinlock waiters now briefly spin and then sleep, allowing a preempted
  holder to run when many processes join at once. The live-stall diagnostic
  accounts for backoff sleep and includes the waiting call site when
  `std::source_location` is available.
- Added `receive_owned<T>()` to retain the full frame of a generated message
  and bounded async RPC result retrieval with `get_until(deadline)`.
- Added optional separate compilation. Set `SINTRA_BUILD_COMPILED=ON` and
  link `sintra::sintra_compiled` to compile the core once. The header-only
  target remains the default.
  [CMake integration](https://github.com/imakris/sintra/blob/v2.0.0/docs/guide.md#cmake-integration)

### Upgrading from v1.2.0

- The shared-memory ring ABI moved from `1` to `12`. Please rebuild and upgrade
  all participants in a swarm together. Run managed swarm participants under
  the same OS account so they can access the private IPC resources.
- Call `spawn_swarm_process()` from the coordinator and make one call per
  child in place of `Spawn_options::count`. Each call returns
  `Managed_child_custody`. Replace `wait_for_instance_name`
  and `wait_timeout` with `readiness_instance_name` and an explicit
  `wait_for_readiness_until(deadline)` on the returned custody. If final
  custody settlement leaves `shutdown()` incomplete, settle the custody and
  retry `shutdown()` sequentially.
  [Spawn and readiness](https://github.com/imakris/sintra/blob/v2.0.0/docs/reference/spawn_swarm_process.md)
  [Shutdown](https://github.com/imakris/sintra/blob/v2.0.0/docs/reference/shutdown.md)
- To launch a peer manually, call `create_external_process_invitation()` on
  the coordinator and pass the returned `sintra_args()` to the new process.
  [External process admission](https://github.com/imakris/sintra/blob/v2.0.0/docs/reference/external_process_invitation.md)
- `Rpc_handle` removed public `wait()`, `wait_until()`, `ready()`, `state()`,
  and `abandon()`, along with the public wait and completion enums. Retrieve
  results with `get()` or `get_until(deadline)`. Where stop tokens are
  available, `get(stop)` lets the caller stop waiting. Destroy or replace a
  pending handle to drop the result. A `sintra::rpc_timeout` from `get_until()`
  means the result was abandoned locally. Remote execution can continue.
  [Async RPC results](https://github.com/imakris/sintra/blob/v2.0.0/docs/reference/rpc_handle.md)
- Replace `barrier_completed(seq)` with `seq != sintra::invalid_sequence`.
  Use `receive_owned<T>()` for generated messages with variable fields. By-value
  `receive<T>()` now rejects those types at compile time.
  [Barriers](https://github.com/imakris/sintra/blob/v2.0.0/docs/reference/barrier.md)
  [Receive API](https://github.com/imakris/sintra/blob/v2.0.0/docs/reference/receive.md)
- Installed CMake packages now require a full matching version for versioned
  requests. Use `find_package(sintra 2.0.0 CONFIG REQUIRED)` or a versionless
  request.

### Windows crash handling

- The per-thread fault guard also reports hardware faults on Sintra-owned
  threads, including ring readers that run application handlers. Sintra
  consults `UnhandledExceptionFilter` on its two fault paths to let the host
  crash reporter and Windows Error Reporting run. Stack overflow is handled
  directly. If the host repairs the fault, execution resumes without
  dispatching abnormal termination or stopping the readers. The crash
  watchdog bounds the host filter's run time to
  `SINTRA_CRASH_WATCHDOG_GRACE_MS` (default `5000`).
  [Host crash reporting](https://github.com/imakris/sintra/blob/v2.0.0/docs/reference/announce_fatal_windows_exception.md)
- When Sintra ends the process for a hardware fault, it reports the cause on
  stderr and exits with the native exception code, such as `0xC0000005`, when
  an exception record is available. Sintra uses `0xC0000409` when it handles
  `SIGABRT`. `SIGTRAP` uses `3`, and
  `SIGINT` and `SIGTERM` still use `1`. Check for a non-zero failure status
  when you do not need the native cause.
- Define `SINTRA_HAS_SEH` consistently across translation units. It defaults
  to `1` on MSVC and clang-cl and `0` elsewhere. Clang for `*-w64-windows-gnu`
  can opt in with `-DSINTRA_HAS_SEH=1 -fms-extensions`. MinGW GCC lacks the
  per-thread guard.

Thank you to everyone who reported issues and tested the fixes.

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
