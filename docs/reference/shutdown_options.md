# sintra::shutdown_options

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`shutdown_options` configures the standard `shutdown()` protocol. It
currently exposes one field: a coordinator-side hook that runs at a
defined point inside the shutdown phase, after the collective processing
fence and before raw teardown begins.

Signature:

```cpp
struct shutdown_options {
    std::function<void()> coordinator_shutdown_hook;
};
```

Use when:

- The coordinator must perform a bounded final action: write a summary
  file, flush a metric, close an external connection, or persist
  collected state.
- All participants need to wait inside the standard shutdown protocol
  while that action runs, without inventing a parallel termination
  helper.

Contract:

- The hook is invoked only on the coordinator process. Non-coordinator
  participants pass the same options object but their hook is never
  called locally.
- The hook runs after the collective processing-fence barrier and before
  the coordinator's drain-wait. By the time it runs, all participants
  have entered the protocol and message dispatching is in service mode.
- The hook must be coordinator-local. It must not initiate new peer
  coordination, additional barriers, or extra custom protocol steps; the
  runtime owns the surrounding synchronisation.
- If the hook throws, `shutdown(options)` attempts local finalisation and
  then rethrows the exception to the caller. Workers complete the hook-done
  rendezvous via the catch-all path so the protocol does not hang. If
  managed-child custody keeps finalisation incomplete, runtime state is
  retained: settle or terminate the retained custody and retry `shutdown`
  sequentially. The retry skips the completed hook phase; Sintra does not
  retain and rethrow the original hook exception.
- An empty `coordinator_shutdown_hook` (default-constructed) means no
  coordinator-side action runs; workers still enter the same hook-done
  rendezvous so cardinality stays consistent.
- Every supported non-happy-path shutdown should be expressed by adding
  fields to this struct rather than introducing a parallel public helper.

Threading and lifecycle:

- The hook runs on the coordinator thread inside the shutdown protocol.
  Treat it like any other coordinator-thread callback: keep work bounded,
  do not reenter Sintra protocol APIs, and avoid blocking on peer
  responses.
- The shutdown protocol state visible through
  `detail::s_shutdown_state` advances to `coordinator_hook_running`
  while the hook is active and to `coordinator_hook_completed` when it
  returns or throws.

Failures:

- Exceptions thrown by the hook are surfaced after a local finalisation
  attempt. They are not deferred until managed-child custody settles and are
  not stored for a later shutdown retry.

Example source:

- [tests/shutdown_options_test.cpp](../../tests/shutdown_options_test.cpp)
- [tests/shutdown_options_throwing_hook_test.cpp](../../tests/shutdown_options_throwing_hook_test.cpp)
- [tests/shutdown_options_mixed_modes_test.cpp](../../tests/shutdown_options_mixed_modes_test.cpp)

See also:

- [sintra::shutdown](shutdown.md)
- [docs/barriers_and_shutdown.md](../barriers_and_shutdown.md)
