# Agent instructions for Sintra

Before proposing designs, reviewing, or changing ring, transport, dispatch or
lifecycle code, read [docs/design_principles.md](docs/design_principles.md).
Also read [CONTRIBUTING.md](CONTRIBUTING.md) for how capabilities evolve, and
[TESTING.md](TESTING.md) for building and running tests.

These rules are the ones most often violated:

- **The rings and the star topology stay.** Do not propose replacing them, or
  adding executors, inbox queues or admission control, as fixes.
- **Only what was lost may fail.** An eviction or death must never fail an RPC,
  fence or barrier that had no message in the lost range.
- **Exactly one copy out of the ring into stable dispatch storage.** Local
  copies/deserialization into independently owned results are acceptable;
  see the dispatch-buffer decision in the design principles.
- **The coordinator is trusted infrastructure** (a kernel analogy only: Sintra
  is a user-space library and the coordinator has no privileges). Its stalls
  and bugs are not defended against; application code hosted in the
  coordinator process shares its fate.
- **Keep owner requirements, verified facts (`file:line`) and your own
  proposals visibly separate.** Adopt a design only after independent
  reviewers report no blockers.

## CI workflow

Once work is locally committed and the required independent review is clean,
fast-forward push to master to start CI. Land one change at a time.

After the push, check CI at elapsed 2, 5, 10, 15, 25 and 35 minutes, then at
1 hour, 1 hour 30 minutes, 2 hours, 2 hours 30 minutes and 3 hours. Do not poll
between those times. Do not wait for CI to finish: continue useful independent
work while it runs and handle results when they arrive. Stop checking once all
workflows have completed. A workflow still running after two hours is not a
failure by itself.
