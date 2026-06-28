# Sintra API Reference

## Core

| Symbol | Kind | Header | Purpose |
| --- | --- | --- | --- |
| [`sintra::init`](init.md) | function | `<sintra/sintra.h>` | Start the current process as part of a Sintra swarm. |
| [`sintra::make_branches`](make_branches.md) | function | `<sintra/sintra.h>` | Build the branch vector consumed by `init`. |
| [`sintra::process_index`](process_index.md) | function | `<sintra/sintra.h>` | Return the current process branch index. |
| [`sintra::Process_descriptor`](process_descriptor.md) | type | `<sintra/sintra.h>` | Describe a process branch or spawn target. |
| [`sintra::spawn_swarm_process`](spawn_swarm_process.md) | function | `<sintra/sintra.h>` | Add a managed process to a running swarm. |
| [`sintra::join_swarm`](join_swarm.md) | function | `<sintra/sintra.h>` | Join an existing branch at runtime. |
| [`sintra::create_external_process_invitation`](external_process_invitation.md) | function | `<sintra/sintra.h>` | Pre-admit a manually launched process into an existing swarm. |
| [`sintra::disable_debug_pause_for_current_process`](disable_debug_pause_for_current_process.md) | function | `<sintra/sintra.h>` | Opt the current process out of debug pause-on-exit behavior. |

## Publish/Subscribe

| Symbol | Kind | Header | Purpose |
| --- | --- | --- | --- |
| [`sintra::world`](world.md) | function | `<sintra/sintra.h>` | Send a plain value to local and remote recipients. |
| [`sintra::local`](local.md) | function | `<sintra/sintra.h>` | Send a plain value to recipients in the current process. |
| [`sintra::remote`](remote.md) | function | `<sintra/sintra.h>` | Send a plain value to recipients in peer processes. |
| [`sintra::Maildrop`](maildrop.md) | type | `<sintra/sintra.h>` | Streaming value sender returned by `world`, `local`, and `remote`. |
| [`sintra::activate_slot`](activate_slot.md) | function | `<sintra/sintra.h>` | Register a typed message handler. |
| [`sintra::deactivate_all_slots`](deactivate_all_slots.md) | function | `<sintra/sintra.h>` | Remove all slots owned by the current process. |
| [`sintra::receive`](receive.md) | function | `<sintra/sintra.h>` | Block a control thread until a matching message arrives. |

## Transceivers and Messages

| Symbol | Kind | Header | Purpose |
| --- | --- | --- | --- |
| [`sintra::Transceiver`](transceiver.md) | type | `<sintra/sintra.h>` | Base runtime object for identity, slots, send, and RPC plumbing. |
| [`sintra::Derived_transceiver`](derived_transceiver.md) | type template | `<sintra/sintra.h>` | Base for user transceivers with typed message emission. |
| [`SINTRA_MESSAGE`](transceiver_macros.md) | macro | `<sintra/sintra.h>` | Declare a typed message with automatic type id. |
| [`SINTRA_MESSAGE_EXPLICIT`](explicit_type_ids.md) | macro | `<sintra/sintra.h>` | Declare a typed message with a user-pinned type id. |
| [`SINTRA_TYPE_ID`](explicit_type_ids.md) | macro | `<sintra/sintra.h>` | Pin a transceiver type id. |
| [Transceiver messages](transceiver_messages.md) | reference | `<sintra/sintra.h>` | Message declaration and emission contract. |
| [Message payloads](message_payloads.md) | reference | `<sintra/sintra.h>` | Supported value, string, array, and variable-buffer payload rules. |
| [Transceiver macros](transceiver_macros.md) | reference | `<sintra/sintra.h>` | Complete macro list, including reserved internal macro names. |

## RPC and Targeting

| Symbol | Kind | Header | Purpose |
| --- | --- | --- | --- |
| [`SINTRA_RPC`](rpc.md) | macro | `<sintra/sintra.h>` | Export a member function as blocking and async RPC. |
| [`SINTRA_RPC_STRICT`](rpc.md) | macro | `<sintra/sintra.h>` | Export an RPC that always uses the transported path. |
| [`SINTRA_UNICAST`](rpc.md) | macro | `<sintra/sintra.h>` | Export a void fire-and-forget targeted message. |
| [`sintra::Rpc_handle`](rpc_handle.md) | type template | `<sintra/sintra.h>` | Move-only async RPC result handle. |
| [`sintra::rpc_cancelled`](rpc_cancelled.md) | exception | `<sintra/sintra.h>` | Exception for RPC cancellation/drain paths. |
| [`sintra::rpc_timeout`](rpc_timeout.md) | exception | `<sintra/sintra.h>` | Exception for bounded async RPC result timeouts. |
| [`sintra::rpc_unavailable`](rpc_unavailable.md) | exception | `<sintra/sintra.h>` | Exception for target-side RPC unavailability. |
| [`sintra::Resolvable_instance_id`](resolvable_instance_id.md) | type | `<sintra/sintra.h>` | Generated RPC target parameter accepting raw ids or names. |
| [`sintra::Typed_instance_id`](typed_instance_id.md) | type template | `<sintra/sintra.h>` | Type-tagged instance id wrapper. |
| [`sintra::Named_instance`](named_instance.md) | type template | `<sintra/sintra.h>` | Type-tagged named target wrapper. |
| [Instance IDs](instance_ids.md) | reference | `<sintra/sintra.h>` | Instance id layout, wildcards, and helpers. |
| [Type IDs](type_ids.md) | reference | `<sintra/sintra.h>` | Automatic and user-pinned type id helpers. |
| [Explicit type IDs](explicit_type_ids.md) | reference | `<sintra/sintra.h>` | User-facing explicit transceiver/message id macros. |

## Barriers and Lifecycle

| Symbol | Kind | Header | Purpose |
| --- | --- | --- | --- |
| [`sintra::barrier`](barrier.md) | function template | `<sintra/sintra.h>` | Synchronize a process group at a named checkpoint. |
| [`sintra::rendezvous_t`](barrier_modes.md) | tag type | `<sintra/sintra.h>` | Barrier mode for rendezvous only. |
| [`sintra::delivery_fence_t`](barrier_modes.md) | tag type | `<sintra/sintra.h>` | Barrier mode for rendezvous plus delivery drain. |
| [`sintra::processing_fence_t`](barrier_modes.md) | tag type | `<sintra/sintra.h>` | Barrier mode for completed handler side effects. |
| [`sintra::shutdown`](shutdown.md) | function | `<sintra/sintra.h>` | Collective runtime shutdown. |
| [`sintra::shutdown_options`](shutdown_options.md) | type | `<sintra/sintra.h>` | Options for coordinated shutdown. |
| [`sintra::leave`](leave.md) | function | `<sintra/sintra.h>` | Unilateral departure while peers continue. |
| [Recovery](recovery.md) | reference | `<sintra/sintra.h>` | Recovery policy, runner, and crash information APIs. |
| [Lifecycle hooks](lifecycle_hooks.md) | reference | `<sintra/sintra.h>` | Process lifecycle event callbacks and reason values. |

## Diagnostics

| Symbol | Kind | Header | Purpose |
| --- | --- | --- | --- |
| [`sintra::init_error`](init_error.md) | exception | `<sintra/sintra.h>` | Structured initialization failure report. |
| [`sintra::Log_stream`](logging.md) | type | `<sintra/sintra.h>` | RAII log-line builder. |
| [`sintra::log_level`](logging.md) | enum | `<sintra/sintra.h>` | Log severity value. |
| [`sintra::set_log_callback`](logging.md) | function | `<sintra/sintra.h>` | Install a process-wide log sink. |
| [`sintra::get_log_callback`](logging.md) | function | `<sintra/sintra.h>` | Inspect the current log sink. |
| [`sintra::log_raw`](logging.md) | function | `<sintra/sintra.h>` | Emit directly through the current log sink. |
| [`sintra::ls_info`](logging.md) | function | `<sintra/sintra.h>` | Create an info-level `Log_stream`. |
| [`sintra::ls_warning`](logging.md) | function | `<sintra/sintra.h>` | Create a warning-level `Log_stream`. |
| [`sintra::ls_error`](logging.md) | function | `<sintra/sintra.h>` | Create an error-level `Log_stream`. |
| [`sintra::ls_debug`](logging.md) | function | `<sintra/sintra.h>` | Create a debug-level `Log_stream`. |
| [`sintra::Console`](console.md) | type | `<sintra/sintra.h>` | Coordinator-routed console output helper. |
| [`sintra::console`](console.md) | type alias | `<sintra/sintra.h>` | Alias for `Console`, used as `sintra::console()`. |

## Direct Rings

| Symbol | Kind | Header | Purpose |
| --- | --- | --- | --- |
| [`sintra::Ring_W`](rings.md#ring_w) | type template | `<sintra/rings.h>` | Single producer for a shared-memory ring. |
| [`sintra::Ring_R`](rings.md#ring_r) | type template | `<sintra/rings.h>` | Consumer for a shared-memory ring. |
| [`sintra::Ring_R_snapshot`](rings.md#ring_r_snapshot) | type template | `<sintra/rings.h>` | RAII wrapper for `start_reading` / `done_reading`. |
| [`sintra::Ring_R_snapshot_error`](rings.md#ring_r_snapshot_error) | enum | `<sintra/rings.h>` | Non-throwing snapshot status. |
| [`sintra::Range`](rings.md#range) | type template | `<sintra/rings.h>` | Pointer range returned by ring reads. |
| [`sintra::Ring_diagnostics`](rings.md#ring_diagnostics) | type | `<sintra/rings.h>` | Snapshot of ring lag, eviction, and guard counters. |
| [`sintra::aligned_capacity`](rings.md#aligned_capacity) | function | `<sintra/rings.h>` | Round a requested ring capacity to a legal aligned size. |
| [`sintra::get_ring_configurations`](rings.md#get_ring_configurations) | function template | `<sintra/rings.h>` | Enumerate page-aligned ring capacities. |
| [`sintra::make_snapshot`](rings.md#make_snapshot) | function template | `<sintra/rings.h>` | Create a throwing RAII read snapshot. |
| [`sintra::try_snapshot_e`](rings.md#try_snapshot_e) | function template | `<sintra/rings.h>` | Create a non-throwing RAII read snapshot with status. |
| [`sintra::ring_payload_traits`](rings.md#ring_payload_traits) | type template | `<sintra/rings.h>` | Opt a specific non-trivial payload type into ring writes. |
| [`sintra::ring_acquisition_failure_exception`](rings.md#ring_acquisition_failure_exception) | exception | `<sintra/rings.h>` | Ring file, mapping, or ownership acquisition failure. |
| [`sintra::ring_abi_mismatch_exception`](rings.md#ring_abi_mismatch_exception) | exception | `<sintra/rings.h>` | Ring control file or lifecycle anchor was created by an incompatible Sintra ABI. |
| [`sintra::ring_reader_evicted_exception`](rings.md#ring_reader_evicted_exception) | exception | `<sintra/rings.h>` | Reader fell too far behind and was evicted. |

## Headers

| Header | API family |
| --- | --- |
| `<sintra/sintra.h>` | Managed processes, pub/sub, RPC, barriers, lifecycle, diagnostics. |
| `<sintra/rings.h>` | Direct ring helpers without the managed-process layer. |

Implementation headers under `sintra/detail/` are not part of the public
header set. For guide-style explanations and recipe links, see
[Sintra Guide](../guide.md).

## Guides and Notes

| Document | Purpose |
| --- | --- |
| [Sintra Guide](../guide.md) | Narrative guide and recipes for the public API. |
| [Diagnostics guide](../diagnostics.md) | Error-to-cause lookup for compiler diagnostics, exceptions, and runtime symptoms. |
| [README](../../README.md) | Project overview, integration notes, and examples. |
| [Architecture notes](../architecture.md) | Internal runtime architecture and process model notes. |
| [Barriers and shutdown](../barriers_and_shutdown.md) | Lifecycle, shutdown, and barrier coordination details. |
| [Process lifecycle notes](../process_lifecycle_notes.md) | Recovery hooks, lifecycle events, and lifeline ownership notes. |
