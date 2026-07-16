# Process lifecycle overview

Sintra has two coordinator-side lifecycle surfaces:

- `set_lifecycle_handler()` observes a committed process retirement as
  `crash`, `normal_exit`, or `unpublished`.
- `set_recovery_policy()` and `set_recovery_runner()` decide whether and when
  an opted-in managed-child custody receives its next occurrence.

Crash provenance is carried with the exact managed-child or external-reader
generation into the same transaction that removes its publication. The
lifecycle callback runs afterward, outside the publication, group, custody,
and reader locks. Recovery is considered only after publication and
communication retirement, notification enqueueing, and the lifecycle callback.

Recovery belongs to a custody, not to a process instance id. A managed child
calls `enable_recovery()` once to opt in that custody; its recovery occurrences
inherit the opt-in, while a fresh custody starts disabled even if it reuses the
same id. The coordinator retains the structured launch recipe for that exact
custody. A delayed `Recovery_control::spawn()` validates the captured custody
and predecessor occurrence again, so it is inert after release, retirement,
shutdown, or id reuse. Externally attached processes have no managed custody
and are never recovered.

The public contracts, threading rules, failure behavior, and examples are in:

- [`sintra::recovery`](reference/recovery.md)
- [`sintra::set_lifecycle_handler`](reference/lifecycle_hooks.md)
- [`sintra::spawn_swarm_process`](reference/spawn_swarm_process.md)
- [`sintra::create_external_process_invitation`](reference/external_process_invitation.md)

Lifeline ownership, collective shutdown, and unilateral departure are separate
from these callbacks; see [Barriers and shutdown semantics](barriers_and_shutdown.md).
