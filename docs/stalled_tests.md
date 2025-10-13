# Investigation: Sintra tests stalling under run_tests.py

## Summary
Several multi-process tests in the suite (`sintra_barrier_flush_test`, `sintra_basic_pubsub_test`, `sintra_ping_pong_multi_test`, and `sintra_rpc_append_test`) consistently time out when they are launched through `tests/run_tests.py`. Direct execution of the same binaries finishes quickly. The investigation shows that the worker processes complete their work and write their results, but all processes remain blocked on the final `_sintra_all_processes` barrier.

## Common patterns in the hanging tests
* Each test spawns multiple additional processes by passing a list of `sintra::Process_descriptor` entries to `sintra::init`, creating a shared process group that includes the parent and its workers.【F:tests/barrier_flush_test.cpp†L256-L263】【F:tests/basic_pub_sub.cpp†L291-L296】【F:tests/ping_pong_multi_test.cpp†L198-L206】【F:tests/rpc_append_test.cpp†L184-L194】  
* All of them coordinate shutdown using `sintra::barrier(..., "_sintra_all_processes")` after their work is finished.【F:tests/barrier_flush_test.cpp†L176-L196】【F:tests/basic_pub_sub.cpp†L219-L255】【F:tests/ping_pong_multi_test.cpp†L134-L207】【F:tests/rpc_append_test.cpp†L129-L194】  
* They write their observable results to files in a temporary directory selected via the shared `SINTRA_TEST_SHARED_DIR` environment variable before waiting on the all-processes barrier.【F:tests/barrier_flush_test.cpp†L179-L181】【F:tests/basic_pub_sub.cpp†L208-L255】【F:tests/ping_pong_multi_test.cpp†L183-L185】【F:tests/rpc_append_test.cpp†L169-L174】  

## Reproducing the hang
Running the test runner with `--preserve-stalled-processes` makes it straightforward to capture the stalled processes for inspection:

```bash
python tests/run_tests.py \
  --repetitions 1 \
  --timeout 10 \
  --test sintra_basic_pubsub_test \
  --build-dir ../build \
  --config Release \
  --verbose \
  --preserve-stalled-processes
```

The runner reports that the parent process (PID 7605 in one run) stalled and keeps it alive for debugging.【25579c†L1-L4】  Listing the processes afterwards shows the parent plus the spawned branches still running, all re-parented under PID 1 after the runner exits.【b5c6f8†L1-L5】  Even though they are stuck, the shared directory contains the expected artifacts and result files indicating that the functional part of each worker completed successfully before the hang.【c384c0†L1-L2】【a63453†L1-L2】  This points to the `_sintra_all_processes` barrier never releasing under the test-runner environment.

## Hypothesis
The common barrier usage suggests that the processes are waiting for the coordinator to flush the inter-process channels so the `_sintra_all_processes` barrier can resolve. The fact that every participant reaches the barrier (as evidenced by the presence of the result files) implies that all members of the group checked in but the coordinator-side flush did not complete, so the barrier never advances and the run times out after the configured 30 seconds. Further investigation should focus on why `Process_group::rpc_barrier` or the subsequent flush logic does not finish when the tests are launched through `run_tests.py`.

## Root cause and fix
Running the suite under Python revealed that once a multi-process binary times out and is terminated with `SIGKILL`, all of the spawned workers remain as zombie processes. The existing `is_process_alive` helper only checked `kill(pid, 0)`, which reports success even for zombies, so the coordinator’s `scavenge_orphans()` logic never reclaimed their reader slots. Subsequent runs would therefore stall at the `_sintra_all_processes` barrier because the flush queue waited forever on readers that could no longer make progress.【F:include/sintra/detail/ipc_rings.h†L296-L332】【F:include/sintra/detail/ipc_rings.h†L935-L964】

Updating the Linux-specific `is_process_alive` implementation to read `/proc/<pid>/stat` allows the runtime to treat zombie (`'Z'`) and dead (`'X'`) processes as absent, letting `scavenge_orphans()` drop their guards and unblock the barrier flush.【F:include/sintra/detail/ipc_rings.h†L296-L332】 After this change, forcibly killing a hung test with `pkill -9 sintra` followed by rerunning the binary from Python succeeds instead of timing out, confirming that the stale barrier participants are now released.【e711a4†L1-L13】

