# Sintra Diagnostics Guide

Use this page when you already have a compiler error, exception, or runtime
symptom and need the rule that explains it. It is a reverse index into the
guide and reference pages.

| Symptom | Likely cause | Corrective path |
| --- | --- | --- |
| Compile error mentioning `Unsupported message argument type` | A top-level maildrop value, RPC argument, or RPC return does not satisfy the payload contract. `SINTRA_MESSAGE` fields are stored exactly as written and should use the explicit field types documented in the payload guide. | Check [Message payloads](reference/message_payloads.md). Use a trivial standard-layout value, a Sintra message element, or a variable-buffer-compatible top-level argument. For transceiver message fields, use wrappers such as `sintra::message_string` or `sintra::typed_variable_buffer<T>`. |
| Compile error mentioning a non-const reference RPC parameter | An exported RPC takes a mutable reference such as `T&`. | Change the API to return a value or send a response message. See [RPC contract](reference/rpc.md). |
| Compile error mentioning a reference RPC return type | An exported RPC returns `T&` or `const T&`. | Return by value. See [RPC contract](reference/rpc.md). |
| Compile error from the generated async wrapper for `SINTRA_UNICAST` | `rpc_async_<method>` was called for a fire-and-forget unicast export. | Use the blocking generated `rpc_<method>` form, or use `SINTRA_RPC_STRICT` when a real async result is required. See [RPC](reference/rpc.md). |
| `sintra::init_error` with `cause::spawn_failed` | A branch process could not be started, commonly because the binary path or arguments are wrong. | Inspect `diagnostic_report()` and the failed process list. See [sintra::init_error](reference/init_error.md) and [sintra::init](reference/init.md). |
| `sintra::init_error` with `cause::barrier_timeout` | A spawned branch did not reach the startup barrier in time. | Check branch startup failures, early exits, or mismatched process arguments. See [sintra::init_error](reference/init_error.md). |
| `Attempted to make an RPC call using an invalid instance ID.` | A string target did not resolve, or a raw id was `invalid_instance_id`. | Call `assign_name(...)` before using a string target, or exchange and use `instance_id()`. See [Resolvable_instance_id](reference/resolvable_instance_id.md) and [Transceiver](reference/transceiver.md). |
| `sintra::rpc_unavailable` | The target instance was unpublished, destroyed, shutting down, or its process exited. | Treat the target as unavailable and reacquire or skip it. See [sintra::rpc_unavailable](reference/rpc_unavailable.md). |
| `sintra::rpc_cancelled` | A caller waiting for an RPC was unblocked by shutdown, drain logic, coordinator loss, or process loss before a reply arrived. | Decide whether caller-side cancellation is acceptable and retry only if the protocol still has a live target. See [sintra::rpc_cancelled](reference/rpc_cancelled.md). |
| Slot never fires | The receiver did not activate the slot before the sender published, the slot type does not match the sent value, or a sender filter excludes the sender. | Add a pre-send barrier, check the handler parameter type, and check any `Typed_instance_id<T>` filter. See [activate_slot](reference/activate_slot.md) and [Publish/Subscribe](guide.md#publishsubscribe). |
| Messages stop while a handler is running | A slot or RPC handler is blocking the reader thread, commonly by calling `receive<T>()` or waiting on a protocol that needs the same reader. | Move blocking waits to a control thread or use another slot/RPC continuation. See [receive](reference/receive.md) and [Threading and Reentrancy](guide.md#threading-and-reentrancy). |
| Handler side effects are not visible after `barrier()` returns | The default `delivery_fence_t` proves delivery drain, not completed peer handler side effects. | Use `barrier<sintra::processing_fence_t>(...)` from a control thread. See [Barrier modes](reference/barrier_modes.md). |
| `ring_abi_mismatch_exception` | A direct ring control file was created by an incompatible Sintra ABI. | Remove the stale ring files or use matching builds. See [Direct rings](reference/rings.md). |
| `ring_reader_evicted_exception` | A direct ring reader fell too far behind the writer. | Increase capacity, read more frequently, or handle eviction through `try_snapshot_e`. See [Direct rings](reference/rings.md). |
| `sintra::variable_buffer overflow` | A variable-size payload is too large to encode in one message frame. | Split the payload, send an application-level chunk protocol, or redesign the message shape. See [Message payloads](reference/message_payloads.md). |
| `assign_name(...)` returns `false` | The coordinator rejected the publication because the name is empty or already taken, the transceiver was already published, or the per-process publication limit was reached. | Choose a non-empty unique name and avoid publishing the same transceiver twice. See [Transceiver](reference/transceiver.md). |

Related checklist pages:

- [Common Mistakes](guide.md#common-mistakes)
- [Message payloads](reference/message_payloads.md)
- [Barriers and shutdown](barriers_and_shutdown.md)
