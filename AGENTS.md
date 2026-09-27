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
- **Exactly one copy out of the ring.** A double copy is a defect.
- **The coordinator is the kernel.** Its stalls and bugs are not defended
  against; code hosted in it runs in kernel mode.
- **Keep owner requirements, verified facts (`file:line`) and your own
  proposals visibly separate.** Adopt a design only after independent
  reviewers report no blockers.
