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

Managed_child_custody spawn_swarm_process(const Spawn_options& options);

Managed_child_custody_observation observe_managed_child(
    const Managed_child_custody& custody);
Managed_child_custody_observation release_managed_child(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline);
Managed_child_custody_observation retry_managed_child_release(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline);
Managed_child_custody_observation wait_managed_child(
    const Managed_child_custody& custody,
    std::chrono::steady_clock::time_point deadline);
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
- `wait_for_instance_name` is optional. Readiness resolution runs as
  Sintra-owned work. With a positive `wait_timeout`, the caller waits only on
  the custody record's monotone notification through that deadline; timeout
  returns accepted, incomplete custody and starts failed-readiness cleanup.
  A zero timeout retains the unbounded legacy readiness wait intentionally.
- `lifetime` controls the lifeline policy applied to the child (see
  `Lifetime_policy`).
- A non-empty return handle means Sintra accepted durable logical custody
  before OS creation authority. `observe_managed_child()` separately reports
  confirmed readiness, admitted/created/exited occurrence counts, and release
  state. OS-create failure remains an accepted no-child record rather than a
  fabricated native identity.
- Calls made while a lifecycle teardown protocol is active are rejected and
  return an empty handle with a warning logged; rejection creates no child.

Threading and lifecycle:

- Call from a top-level user thread. Acceptance takes the teardown admission
  lock; deadline-facing waits do not enter coordinator work and wait only on
  custody notifications.
- Successful spawns participate in subsequent barriers and coordinator
  membership immediately. The wait variant is the way to gate later code on
  the participant having published its name.
- `readiness_reached` means the coordinator observed the requested name; it
  does not imply release or any later retirement fact.
- `release_managed_child()` closes recovery before requesting retirement and
  returns only confirmed facts by its absolute deadline. An incomplete return
  retains ownership. Retry and wait operate on the same opaque record; they do
  not reconstruct authority from a process id or name.
- Complete release joins authoritative exact-occurrence publication and
  communication retirement with confirmed OS exit for every admitted
  occurrence. Dropping the handle does not drop Sintra's retained obligation.

Failures:

- Returns an empty handle (with a logged error) when validation or lifecycle
  admission rejects before custody acceptance.
- Readiness timeout or resolution failure returns accepted custody with
  `readiness_reached == false`; it is not a spawn-count failure.

Example source:

- [tests/spawn_wait_test.cpp](../../tests/spawn_wait_test.cpp)
- [tests/spawn_detached_test.cpp](../../tests/spawn_detached_test.cpp)
- [tests/lifeline_basic_test.cpp](../../tests/lifeline_basic_test.cpp)

See also:

- [sintra::join_swarm](join_swarm.md)
- [sintra::create_external_process_invitation](external_process_invitation.md)
- [sintra::Process_descriptor](process_descriptor.md)
- [sintra::init](init.md)
