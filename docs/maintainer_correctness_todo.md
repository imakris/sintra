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

## Correctness TODO

- Redesign `Message<T>` lifetime. It still inherits the body type and
  placement-news over the already constructed base subobject. Move toward
  composition/storage and provide compatibility accessors if needed.
- Audit `Transceiver::destroy()`. Local pointer-map cleanup currently depends
  on runtime/coordinator/reader availability because the function can return
  before erasing `m_local_pointer_of_instance_id`.
- Establish and document coordinator lock ordering. Review paths that can take
  `m_groups_mutex -> m_publish_mutex` and `m_publish_mutex -> m_groups_mutex`.
- Convert coordinator manual `lock()`/`unlock()` paths around message writes to
  RAII locking, especially deferral and wait-for-instance paths.
- Guard `m_processes_in_initialization` consistently. `join_swarm()` reads it
  while holding `m_publish_mutex`, but other paths use `m_init_tracking_mutex`.
- Define a locking policy for recovery membership state such as
  `m_requested_recovery` and `m_external_attached_processes`.
- Make `make_process_instance_id()` allocation use a single atomic
  fetch/check operation, or move process-id allocation under coordinator
  membership ownership.
- Revisit detached `Process_message_reader` threads and destructor-time
  `exit(1)`. Prefer joinable ownership or a process-supervision policy outside
  the object destructor.
- Split `Ring_R::done_reading()` shutdown signalling from snapshot release.
  A redundant public `done_reading()` currently requests stop.
- Add backoff/yield helpers to remaining spin loops, matching the more careful
  `Message_ring_R::fetch_message()` spin/yield pattern.

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
- Stop propagating debug/optimization flags through the public `sintra`
  interface target. Restrict those flags to tests/examples/internal targets.
- Fix the test CMake active-test count message; it currently prints a blank
  count.
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
- Document coordinator lock order and forbidden lock/callback patterns.
- Document runtime lifecycle phases and the `shutdown_protocol_state` state
  machine.
- Document internal child-process CLI arguments parsed by the runtime.
- Document important globals/TLS variables, their owner thread, lifetime, and
  required protocol phase.
- Add a test map: which focused tests to run when touching rings, message
  framing, coordinator barriers, shutdown/finalize, recovery, and external
  attach.
