# sintra::spawn_swarm_process

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`spawn_swarm_process` adds one managed process to a running swarm
after `init` has completed. It launches the requested binary with the
required Sintra arguments, optionally waits for a named instance to appear,
and applies the configured lifeline policy. The starter process or any
running participant may call it; child processes inherit the swarm and
coordinator id from the parent.

Signature:

```cpp
struct Spawn_options
{
    std::string                binary_path;
    std::vector<std::string>   args;
    instance_id_type           process_instance_id = invalid_instance_id;
    std::string                wait_for_instance_name;
    std::chrono::milliseconds  wait_timeout{0};
    Lifetime_policy            lifetime;
};

size_t spawn_swarm_process(const Spawn_options& options);
```

Use when:

- A new participant must join a running swarm dynamically (a worker pool
  scaling out, a late-arriving subscriber, a tool launching a helper).
- The caller needs to block until the new participant publishes a known
  instance name (`wait_for_instance_name`), with an optional bounded wait
  (`wait_timeout`).

Contract:

- `binary_path` must be non-empty. The spawned program runs that binary,
  receives `args` as positional arguments (Sintra inserts the binary name
  as `argv[0]` if not already present), and gets `--swarm_id`,
  `--instance_id`, and `--coordinator_id` appended automatically.
- `process_instance_id` defaults to a fresh process instance id. Setting it
  pins the new process to a specific id; the caller must guarantee the id
  is not already in use.
- `wait_for_instance_name` is optional. When set, the call blocks until the
  coordinator resolves that name. With `wait_timeout` zero, the wait blocks
  indefinitely on the coordinator's `rpc_wait_for_instance`. With a
  positive `wait_timeout`, the call polls `rpc_resolve_instance` with
  exponential backoff up to the deadline.
- `lifetime` controls the lifeline policy applied to the child (see
  `Lifetime_policy`).
- The return value is `1` on successful spawn. When the wait phase
  fails (timeout, exception, or the resolved id remained invalid), the
  function returns zero even when the OS spawn itself succeeded.
- Calls made while a lifecycle teardown protocol is active are rejected and
  return zero with a warning logged.

Threading and lifecycle:

- Call from a top-level user thread, not from a message handler or
  post-handler callback. The implementation takes the teardown admission
  lock and issues coordinator RPCs, both of which require a normal control
  context.
- Successful spawns participate in subsequent barriers and coordinator
  membership immediately. The wait variant is the way to gate later code on
  the participant having published its name.
- A successful return with `wait_for_instance_name` set means the
  coordinator has observed the name; it does not guarantee the child has
  finished its own slot activations beyond that publication step.

Failures:

- Returns zero (with a logged error) when `binary_path` is empty.
- Returns zero when wait fails: the coordinator returned
  `invalid_instance_id`, `wait_timeout` elapsed, or the wait RPC threw.

Example source:

- [tests/spawn_wait_test.cpp](../../tests/spawn_wait_test.cpp)
- [tests/spawn_detached_test.cpp](../../tests/spawn_detached_test.cpp)
- [tests/lifeline_basic_test.cpp](../../tests/lifeline_basic_test.cpp)

See also:

- [sintra::join_swarm](join_swarm.md)
- [sintra::create_external_process_invitation](external_process_invitation.md)
- [sintra::Process_descriptor](process_descriptor.md)
- [sintra::init](init.md)
