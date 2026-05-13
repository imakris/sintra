# sintra::join_swarm

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`join_swarm` requests an additional process for an existing branch index in
the running swarm. The coordinator serializes join requests for the same
branch and returns the new instance id, or `invalid_instance_id` when the
join is rejected.

Signature:

```cpp
instance_id_type join_swarm(
    int branch_index,
    std::string binary_name = std::string());
```

Use when:

- A branch declared in the original `init` should grow with another running
  copy.
- A spawned process wants to ask the coordinator to launch a peer at a
  specific branch index without building a `Spawn_options` directly.

Contract:

- `branch_index` must be at least one. The starter process is index zero
  and cannot be joined.
- When `binary_name` is empty, the caller's own binary name (the value the
  managed process was launched with) is used.
- The call requires a valid coordinator id. If the local managed process
  has not registered a coordinator yet, or if a lifecycle teardown is in
  progress, the call returns `invalid_instance_id` immediately.
- A non-`invalid_instance_id` return value is the new process's instance
  id. Callers can use it as an RPC target or wait for a named transceiver
  inside the new process.
- Any exception propagated by the coordinator RPC is suppressed and reported
  by returning `invalid_instance_id`.

Threading and lifecycle:

- Call from a top-level user thread. The implementation takes the teardown
  admission lock and issues an RPC to the coordinator.
- The new process inherits the swarm and coordinator from the existing
  topology. It is subject to the same lifeline and recovery configuration as
  the original branch.

Failures:

- Returns `invalid_instance_id` when:
  - `branch_index` is less than one
  - the runtime is not initialised or teardown admission has been closed
  - the coordinator id is `invalid_instance_id`
  - the coordinator RPC throws

Example source:

- [tests/join_swarm_failure_test.cpp](../../tests/join_swarm_failure_test.cpp)
- [tests/join_swarm_midflight_test.cpp](../../tests/join_swarm_midflight_test.cpp)

See also:

- [sintra::spawn_swarm_process](spawn_swarm_process.md)
- [sintra::create_external_process_invitation](external_process_invitation.md)
- [sintra::init](init.md)
- [sintra::process_index](process_index.md)
