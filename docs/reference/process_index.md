# sintra::process_index

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`process_index` returns the branch index of the current managed process.
Branch index 0 is the starter (the process that called `init` from the
parent program); each declared branch becomes a positive index assigned in
the order the descriptors were passed to `init`. Outside an initialised
swarm the value is `-1`.

Signature:

```cpp
int process_index();
```

Use when:

- Selecting branch-specific behaviour after `init` returns. A
  multi-branch program with one binary commonly switches on
  `process_index()` to route control flow.
- Logging or telemetry that needs to identify which branch produced a line.
- Skipping setup work that should only happen on the starter (for example,
  printing a final summary).

Contract:

- Returns `-1` before `init` has been called and after teardown when no new
  `init` has run.
- Returns `0` in the starter process.
- Returns a positive integer in branches spawned by `init`. The integer
  equals the descriptor's position in the branch vector consumed by `init`.
- The value is set during managed-process startup and stays constant for
  the life of the process.

Threading and lifecycle:

- Reading `process_index()` is safe from any thread. The underlying value
  is a process-wide global initialised before user threads run.

Failures:

- None. The function performs a plain read.

Example source:

- [example/sintra/sintra_example_4_recovery.cpp](../../example/sintra/sintra_example_4_recovery.cpp)

See also:

- [sintra::init](init.md)
- [sintra::make_branches](make_branches.md)
- [sintra::join_swarm](join_swarm.md)
