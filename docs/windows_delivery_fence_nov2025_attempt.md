# Windows delivery-fence investigation (November 2025)

## Summary of the attempt

* Goal: implement the two-phase barrier completion sketched in `windows_delivery_fence_strategy.md` using **delta-based** delivery targets so the coordinator waits for each worker's request backlog before releasing the barrier.
* Approach: extend `Process_group::barrier()` to capture `get_request_leading_sequence()`/`get_request_reading_sequence()` on every arrival, compute `delta = leading - reading`, and defer barrier completions via `run_after_current_handler()` until each target reports `observed >= baseline + delta`.
* The implementation never left the prototype stage. While instrumenting the code the regression test continued to fail (`Coordinator did not observe first marker`), so all code changes were reverted before finishing the run.

## Instrumentation results

To diagnose the failure the prototype logged every recorded delivery target and each satisfaction check. Representative excerpts:

```
[capture] target #4 process=144115188075855873 baseline=2047 delta=198 stream=req
[delivery] satisfied check #0 baseline=2047 delta=198 goal=2245 observed=104985 stream=req
```

Key observations:

* Most arrivals produced `delta = 0` because by the time the coordinator processed the barrier RPC, the corresponding request reader had already drained its backlog. Only one participant per barrier ever showed a non-zero delta.
* The `observed` counter often jumped well beyond the computed goal (e.g. `observed=104985` for `goal=2245`). The backlog drained quickly, yet the regression still reported missing iteration markers.
* Even when the coordinator reported `observed >= baseline + delta`, `barrier_delivery_fence_repro_test` failed in the **next** iteration (`Coordinator did not observe first marker for iteration 2`). Waiting for the recorded delta alone did not prevent the coordinator from missing markers.

## Conclusions

* Capturing absolute sequences and converting them to deltas is not sufficient. Readers that were idle at barrier entry never contribute non-zero deltas, so the coordinator still releases the barrier while other workers are mid-flight.
* The enormous jumps in `observed` suggest the coordinator's counters span multiple epochs; simply subtracting `leading - reading` ignores wrap-around and restart semantics.
* The prototype also revealed that holding the barrier reply via `mark_rpc_reply_deferred()` requires sending the **original** reply (function instance id) to the last arrival; otherwise the RPC never resumes.

Because the instrumentation still pointed to missing markers, the code changes were abandoned to avoid destabilising `main`.

## Suggested next steps

1. Augment the coordinator snapshot with **per-arrival baselines** for both `request_sequence` and `reading_sequence`, and store them alongside the delta. Future waits should check `(current_observed - baseline) >= delta`, not just absolute values.
2. Track whether the coordinator actually processed the backlog by logging the number of messages drained between snapshots. Correlate this with the regression test's iteration counter to confirm that the missing marker originates in the same process that reported a non-zero delta.
3. Consider waiting for **all** participants to publish at least one delivery-progress update before enabling the handshake. Targets with `observed = 0` at barrier entry likely belong to readers that have not fully initialised.

## Prompt for the next attempt

> Revisit the delivery-fence handshake by storing `{baseline, delta}` pairs for each request reader and waiting until `(observed - baseline) >= delta`. Add structured logging that correlates barrier iterations with the drained message counts (e.g. `[barrier] iteration=I process=P backlog_before=X backlog_after=Y`). Validate on `barrier_delivery_fence_repro_test` and keep the instrumentation behind a debug macro so it can be toggled without modifying the code.

