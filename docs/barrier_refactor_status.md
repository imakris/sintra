# Barrier Refactor Implementation Status

The barrier refactor is still in flight. The current codebase contains the
coordinator rendezvous changes that track `(instance_id, boot_id)` pairs and fan
out completions directly to the registered waiters, but several architectural
pieces from `docs/barrier_refactor_architecture.md` remain outstanding:

- Participant-side acknowledgement handling still relies on the legacy detached
  thread that polls the reply ring for completions. We have not yet introduced
  the bounded completion queue that executes after the handler unwinds or the
  explicit completion signal awaited by barrier callers.
- Outbound delivery tracking is limited to the coordinator asking for outbound
  acknowledgements. Per-reader delta bookkeeping, the relevant-reader snapshot,
  and the split timer windows described in the architecture doc still need to be
  implemented.
- Processing-phase acknowledgements currently accept a bare success flag. The
  managed process does not wait on `wait_for_processing_quiescence()` nor does it
  report `(instance_id, boot_id)` in its replies, so restarts are not
  disambiguated.
- Timer management is temporary. The coordinator still relies on fixed
  rendezvous and outbound deadlines and does not yet wire the epoch/sequence
  snapshots or the disarm-on-state-change logic called for in the design.
- Barrier payloads surface the new phase statuses, but legacy test harnesses and
  documentation (for example `docs/NEXT_PROMPT.md`) still reference the previous
  behaviour. We need dedicated regression coverage for outbound and processing
  phases before flipping the new architecture on by default.
- Logging and harness integration for the new processing fence traces (such as
  `log_processing_barrier_state` and `SINTRA_PROCESSING_FENCE_DIR`) is not yet
  automated in `run_tests.py`.

These gaps are tracked so the remaining work can be scheduled explicitly instead
of being hidden behind the partial coordinator refactor.
