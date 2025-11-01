

## Runtime Context

- Barriers always return a deferral to non-final callers. The worker thread that
  invoked the barrier parks inside `Transceiver::call()` waiting on the shared
  `common_function_iid`, while the request reader and reply reader threads remain
  active.
- The coordinator's `barrier_ack_request` RPC is serviced by
  `Managed_process::barrier_ack_request()` on the request reader thread. That
  handler performs the required waits (`Managed_process::flush()` for outbound,
  `wait_for_delivery_fence()`/`wait_for_processing_quiescence()` for processing)
  before attempting to emit a `barrier_ack_response`.
- Because `group_requirement_mask` is the union of every caller's flags, a
  participant that only needs a rendezvous may still have to send outbound or
  processing acknowledgements when another peer requested them.
- The coordinator now defers the final barrier caller whenever outbound or
  processing guarantees are requested and spins up a lightweight timeout monitor
  thread. The monitor polls each barrier's deadlines, marks phases as `timeout`
  when peers never acknowledge, and finalises the barrier without blocking the
  request reader.

## Instrumentation Snapshot (WIP)

- Enable tracing by setting `SINTRA_TRACE_BARRIER=1`. This causes
  `log_barrier_event()` calls in the coordinator and managed processes to stream
  detailed timestamps to stdout and to per-run files in the scratch directory
  (`controller.log`, `worker.log`, etc.).
- `Managed_process::barrier_ack_request()` now logs each acknowledgement request,
  the sequences observed inside `wait_for_delivery_fence()`, and when the reply
  is emitted. `Process_group::barrier()`/`barrier_ack_response()` report every
  deferral, ack arrival, and timeout monitor decision.
- The processing-fence integration test (`tests/processing_fence_test.cpp`)
  records whether the worker's handler actually executed via a shared atomic
  (`handler_ran=true/false`), alongside timestamped controller/worker events.
- Current logs show the processing barrier completing while the worker still
  reports `handler_ran=false`. `wait_for_delivery_fence()` observes only the
  request/reply rings (`observed=955 target=1327` in the traces) and cannot see
  the broadcast `world()` traffic that carries `Work_message`. The ack therefore
  fires before the handler thread runs.

**Outstanding work:**
- Extend the processing ack path to wait for broadcast/event delivery, or hook
  into a positive completion signal from the worker's post-handler queue before
  replying. Until then `sintra_processing_fence_test_debug` remains red.

## Current Experiments

Several code spikes were attempted to break the circular wait, none of which landed cleanly in the current tree. The following observations explain why they did not ship and clarify the remaining work:

- Running the ack lambda synchronously (instead of deferring via `run_after_current_handler`) eliminates the circular dependency, but it also reintroduces the exact spinlock crashes that triggered the refactor in the first place: the sync path touches coordinator-owned containers while the request reader still holds the spinlocked data structures that were being torn down. Without reworking the teardown ordering, that approach is unsafe.

- Redirecting the reply through a new signal (`Managed_process::barrier_ack_notify` -> `Process_group::barrier_ack_signal`) breaks the dependency on `tl_post_handler_function`, but the participants are not allowed to send signals of coordinator types. The type system enforces this (see the `sender_capability` static assertion in `Transceiver::send`). Introducing a dedicated notification signal would require widening the allowed sender set or routing through another transceiver (for example, via a neutral broker). That work was not completed.

- Emitting the notification from a detached thread (to unblock the post-handler queue) sidesteps the `run_after...` constraint, but it pushes coordinator RPCs onto a brand new thread without any of the existing teardown guards. This raises new ordering issues and still relies on the coordinator RPC path we are trying to retire.

Any final solution must deliver the barrier acknowledgement to the coordinator on a path that (a) does not depend on the blocked barrier RPC, and (b) maintains the safety guarantees that motivated the refactor (no spinlocked container access during teardown).
