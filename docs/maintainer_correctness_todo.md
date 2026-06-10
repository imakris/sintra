# Maintainer correctness and refactor TODO

This note captures current internal review findings that should not be lost.
It is not a historical handover; keep it current as items are fixed or
reclassified.

## Recently resolved

- Reader state packing no longer relies on union type punning.
- Variable-buffer construction context is restored with RAII when body
  construction throws.
- Ring control files are initialized in a temporary file before no-clobber
  publish.
- Ring lifecycle attachments are tracked in a persistent lifecycle anchor.
- If data/control files exist without the lifecycle anchor, attach fails closed
  and does not delete them automatically.
- Variable-sized message frames are rounded to `message_frame_alignment`.
- Message readers reject misaligned frame lengths.
- The ring ABI version was bumped for the message-framing change.
- Raw `variable_buffer` payloads whose element alignment exceeds the message
  frame alignment are rejected at compile time.
- Producer-side message-frame sizing now uses the same exclusive single-octile
  limit as readers.
- Test scratch paths are shortened to avoid Windows path-length failures.
- Coordinator lock ordering is established and documented in `coordinator.h`
  (`m_external_process_invitations_mutex` -> `m_groups_mutex` ->
  `m_publish_mutex` -> `m_init_tracking_mutex`). `unpublish_transceiver()`
  acquires `m_groups_mutex` before `m_publish_mutex` for process unpublishes,
  keeping the registry erase, group/barrier removal, lifecycle/recovery
  decision and the unpublished broadcast atomic with respect to same-slot
  re-publishes, and `claim_external_process_invitation()` no longer acquires
  `m_publish_mutex` under `m_init_tracking_mutex`. The previously possible
  ABBA deadlock against `make_process_group()` is reproduced (pre-fix) and
  guarded by `coordinator_lock_order_test`.
- Coordinator manual `lock()`/`unlock()` around message writes
  (`wait_for_instance()`, `Process_group::barrier()`) is converted to RAII
  locking; a throwing ring write can no longer leak a locked mutex.
- `m_processes_in_initialization` is consistently guarded by
  `m_init_tracking_mutex` (the `join_swarm()` capacity check used to read it
  under `m_publish_mutex`).
- Recovery membership state has a locking policy: `m_requested_recovery` is
  guarded by `m_lifecycle_mutex` (which already guards the recovery
  policy/runner), and `m_external_attached_processes` is guarded by (and
  documented under) `m_publish_mutex`.
- `make_process_instance_id()` allocates and range-checks in a single atomic
  fetch/check step.
- The remaining ring reader-lock spin loops use a shared pause/yield backoff
  helper (`detail::Spin_backoff`, in `ipc/spinlock.h`), matching the
  `Message_ring_R::fetch_message()` pattern; `spinlock::lock()` now also
  issues a CPU pause while spinning.
- Debug/optimization flags are no longer propagated through the public
  `sintra` INTERFACE target; internal example targets opt in via
  `sintra_dev_flags`, and tests keep their own per-suite flags.

## Correctness TODO

- Redesign `Message<T>` lifetime. It still inherits the body type and
  placement-news over the already constructed base subobject. Move toward
  composition/storage and provide compatibility accessors if needed.
- Audit `Transceiver::destroy()`. Local pointer-map cleanup currently depends
  on runtime/coordinator/reader availability because the function can return
  before erasing `m_local_pointer_of_instance_id`.
- Revisit detached `Process_message_reader` threads and destructor-time
  `exit(1)`. Prefer joinable ownership or a process-supervision policy outside
  the object destructor.
- Split `Ring_R::done_reading()` shutdown signalling from snapshot release.
  A redundant public `done_reading()` currently requests stop.
- Add a debug-build lock-rank assertion for the coordinator mutex hierarchy
  documented in `coordinator.h`, so order violations fail loudly instead of
  relying on the comment and on `coordinator_lock_order_test`'s two
  instrumented stages.
- Consolidate `detail::spin_pause()` with `backoff_yield()` in
  `ipc/semaphore.h`, and add MSVC ARM64 support (`YieldProcessor`/`_M_ARM64`)
  to the shared implementation.

## Message and ring protocol TODO

- Keep message-frame protocol checks centralized. Producers and consumers
  should continue to use the same alignment and maximum-size rules.
- Consider moving message-frame constants into a small internal header such as
  `detail/messaging/message_frame.h` once the message/ring layering is split.
- Clarify `max_message_length` in `config.h`. It is included in the ring ABI
  fingerprint but the effective message-frame limit is the single-octile
  message ring limit.
- Add explicit documentation for message-frame invariants:
  `bytes_to_next_message` alignment, single-octile limit, prefix magic, and
  variable-buffer construction context.
- Add a reader-side message-start alignment validation if future corruption
  tests show it catches useful failures.

## Build and packaging TODO

- Consider moving non-template runtime implementation out of the public include
  path and keeping header-only mode as an opt-in. Current public includes pull
  in the coordinator, managed-process runtime, IPC primitives, and message
  readers.
- Revisit release-with-debug test flags. `-O0 -g -DNDEBUG` is not a normal
  release-with-debug configuration.
- Add a non-blocking strict-warning CI lane before making it blocking:
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow`.
- Exclude Python bytecode, CI trigger sentinels, and generated reference-site
  output from source release archives unless intentionally packaging generated
  docs.

## Refactor TODO

- Introduce RAII request/reply write transactions so `write()` and
  `done_writing()` cannot be separated by throwing coordinator or dispatcher
  code.
- Consolidate duplicated unavailable-reply, deferral, barrier-completion, and
  RPC-reply emission helpers.
- Extract lifecycle anchor protocol from `rings.h` into a focused internal
  module with its invariants documented at the top.
- Extract signal bridge, lifeline, spawn-argument parsing, and runtime argument
  construction from `managed_process_impl.h`.
- Add C++20 concepts for message types, transceiver-like types,
  variable-buffer-compatible containers, and trivially serializable payloads.
- Reduce public `using std::...` aliases in namespace `sintra`.
- Replace `Named_instance : std::string` with composition in a planned API
  cleanup.
- Add a correctly spelled `all_remote_processes_wildcard` alias and deprecate
  the misspelled public name if it is considered public API.

## Documentation TODO

- Add an internal source map for message send, RPC, shutdown, direct rings,
  recovery, and external process invitation flows.
- Document forbidden lock/callback patterns beyond the coordinator lock order
  now described in `coordinator.h` (e.g., which user callbacks may run while
  coordinator locks are held).
- Document runtime lifecycle phases and the `shutdown_protocol_state` state
  machine.
- Document internal child-process CLI arguments parsed by the runtime.
- Document important globals/TLS variables, their owner thread, lifetime, and
  required protocol phase.
- Add a test map: which focused tests to run when touching rings, message
  framing, coordinator barriers, shutdown/finalize, recovery, and external
  attach.
