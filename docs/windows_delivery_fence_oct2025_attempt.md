# Windows delivery fence investigation (October 2025)

## Context

I attempted to implement the remote-drain handshake proposed in previous
investigations. The idea was to capture each caller's request-ring leading
sequence at barrier entry, defer the final barrier completions on the
coordinator, and only emit them after the coordinator's request readers had
advanced to the recorded targets. The coordinator would schedule a
`run_after_current_handler` task to run on the request thread and wait on
`Managed_process::wait_for_remote_delivery()` before invoking
`emit_barrier_completions()`.

## Work performed

* Extended `Process_group::barrier()` to accept `request_leading_sequence` and
  a `require_delivery_fence` flag. Last arrivals captured the incoming sequences
  per participant and deferred completions via `run_after_current_handler`.
* Added a new helper `Managed_process::wait_for_remote_delivery()` to block on
  arbitrary request-stream targets by reusing the existing delivery-progress
  machinery.
* Updated `detail::barrier_dispatch(delivery_fence_t, ...)` to pass the caller's
  outbound leading sequence together with `require_delivery_fence = true`.
* Instrumented both the dispatcher and coordinator to dump the captured
  sequences and the state of the handshake.
* Rebuilt `sintra_barrier_delivery_fence_repro_test_release` and ran it
  repeatedly under the instrumented build.

## Observations

* All arrivals printed `require_delivery=0` with
  `request_seq=18446744073709551615 (invalid_sequence)`. The dispatcher-side
  logging never fired, confirming that the RPC traffic reaching
  `Process_group::barrier()` was coming from call sites that never opted into
  the delivery-fence path (notably the direct `Process_group::rpc_barrier()`
  usages in `Managed_process::branch()` and the startup barriers).
* Because of the above, the coordinator never entered the deferred-completion
  branch; the new helper was effectively idle. The repro test still stalled the
  coordinator after the initial "ready" barrier, which matches the original
  failure mode.
* Forcing the handshake during startup barriers is not correct-the map of
  request readers is incomplete during early initialization and the wait spins
  forever (reproducing the "315 vs 0" stall recorded in the May 2025 notes).
* Injecting tracing directly into `delivery_fence_t` confirmed that the helper
  would only make progress once the dispatcher actually passed
  `require_delivery_fence = true` together with a finite target.

## Roadblocks

* The only call sites that currently provide the "delivery fence" semantics are
  the templated `sintra::barrier<delivery_fence_t>` wrappers. Direct calls to
  `Process_group::rpc_barrier` bypass the new parameters. The repro test uses
  the high-level wrapper, but due to earlier instrumentation failures I did not
  complete a full run that proves the handshake succeeds when the dispatcher
  participates.
* `Managed_process::wait_for_remote_delivery()` needs more guardrails. During
  startup the coordinator's reader threads are still in `READER_SERVICE` state;
  attempting to wait on them blocks indefinitely unless readers are filtered by
  `reader.state() == READER_NORMAL`.
* Repeated attempts to gather live traces were hampered by the proliferation of
  spawned child processes (`--branch_index` workers). Killing the repro test
  without a clean shutdown leaves numerous orphans that have to be cleared with
  `pkill -f sintra_barrier_delivery_fence_repro_test_release` before the next
  run.

## Suggested next steps

1. Teach `detail::barrier_dispatch(delivery_fence_t, ...)` to emit a unique log
   line (or trigger a counter) so we can assert that the high-level API actually
   exercises the delivery-fence path. The repro should be run to completion with
   this instrumentation enabled.
2. When the dispatcher does set `require_delivery_fence = true`, gather the
   `(process_id, request_target, observed_progress)` triples so we can verify the
   mapping between writer sequences and the coordinator's progress counters.
3. Confirm that the deferred completions run via `run_after_current_handler`
   still respect the single-writer contract on the reply ring. If necessary,
   consider collecting the completions vector and writing it via
   `emit_barrier_completions()` after re-locking `m_call_mutex`, as originally
   planned.
4. Once the dispatcher side is proven to call into the new path, rerun
   `sintra_barrier_delivery_fence_repro_test_release` end-to-end to validate that
   the handshake eliminates the missing-marker failure on Windows.

## Prompt for the next attempt

> Extend `detail::barrier_dispatch(delivery_fence_t, ...)` with lightweight
> tracing so we can prove that `sintra::barrier()` really toggles the new
> handshake. Run `sintra_barrier_delivery_fence_repro_test_release` and capture
> the `(process_id, require_delivery, request_sequence, observed_sequence)` pairs
> reported by the coordinator. Once you confirm that every delivery-fence call
> supplies a finite request target, focus on tightening
> `Managed_process::wait_for_remote_delivery()` so it tolerates startup readers
> and then re-evaluate the Windows repro.

