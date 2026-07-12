# sintra::spawn_swarm_process

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`spawn_swarm_process` adds one managed process to a running swarm
after `init` has completed. It launches the requested binary with the
required Sintra arguments, optionally observes a named instance as readiness,
and applies the configured lifeline policy. Only the process hosting the local
coordinator may call it; child processes inherit the swarm and coordinator id
from that process.

Signature:

```cpp
struct Spawn_options
{
    std::string                binary_path;
    std::vector<std::string>   args;
    instance_id_type           process_instance_id = invalid_instance_id;
    std::string                readiness_instance_name;
    Lifetime_policy            lifetime;
};

Managed_child_custody spawn_swarm_process(const Spawn_options& options);

class Managed_child_custody
{
public:
    Managed_child_status status() const;
    Managed_child_status wait_ready_until(
        std::chrono::steady_clock::time_point deadline) const;
    Managed_child_status release_until(
        std::chrono::steady_clock::time_point deadline) const;
    Managed_child_status terminate_until(
        std::chrono::steady_clock::time_point deadline) const;
};
```

Use when:

- A new participant must join a running swarm dynamically (a worker pool
  scaling out, a late-arriving subscriber, a tool launching a helper).
- The caller needs to observe whether the new participant publishes a known
  instance name (`readiness_instance_name`) through an explicit absolute
  deadline.

Contract:

- `binary_path` must be non-empty. The spawned program runs that binary,
  receives `args` as positional arguments (Sintra inserts the binary name
  as `argv[0]` if not already present), and gets `--swarm_id`,
  `--instance_id`, and `--coordinator_id` appended automatically.
- `process_instance_id` defaults to a fresh process instance id. Setting it
  pins the new process to a specific valid process id. Transceiver ids,
  wildcard ids, ids with an invalid process component, pending invitation ids,
  and ids with unresolved child custody are rejected before acceptance.
- `readiness_instance_name` is optional. When configured, occurrence setup and
  readiness resolution run as Sintra-owned work and `spawn_swarm_process`
  returns the accepted handle immediately. `wait_ready_until()` waits only on
  the custody record's monotone notification through its absolute steady-clock
  deadline. Deadline expiry returns accepted, incomplete custody and requests
  adverse cleanup after leaving the record lock.
- `lifetime` controls the lifeline policy applied to the child (see
  `Lifetime_policy`).
- A return handle that converts to `true` means Sintra accepted durable logical
  custody before OS creation authority. `Managed_child_custody::status()` separately
  reports confirmed readiness, admitted/created/exited occurrence counts, and
  release state. OS-create failure remains an accepted no-child record rather
  than a fabricated native identity.
- Once custody is accepted, synchronous setup failures do not escape and discard
  the handle. A failure before native creation returns accepted no-child custody
  and requests release. A failure after native creation returns accepted custody
  while retained cleanup continues through exact exit confirmation.
- Calls made while a lifecycle teardown protocol is active are rejected and
  return an empty handle with a warning logged; rejection creates no child.

Threading and lifecycle:

- Call from a top-level user thread in the coordinator process. Worker-process
  calls are rejected before acceptance and cannot create a child. Acceptance
  takes the teardown admission lock; `wait_ready_until()` does not enter
  coordinator work and waits only on custody notifications.
- Successful spawns participate in subsequent barriers and coordinator
  membership once setup completes. `wait_ready_until()` is the way to gate
  later code on the exact participant having published its readiness name.
- `readiness_reached` means the coordinator observed the requested name; it
  does not imply release or any later retirement fact.
- `Managed_child_custody::release_until()` closes recovery before requesting retirement and
  waits for graceful/natural retirement, returning only confirmed facts by its
  absolute deadline. It does not release the child's lifeline or initiate
  adverse cleanup. The operation is idempotent: another call requests the same
  graceful release and waits on the same retained custody through its new
  absolute deadline.
- `Managed_child_custody::terminate_until()` is the explicit adverse-path escalation. It closes
  recovery and monotonically asks Sintra's retained custody owner to drain the
  exact admitted occurrences, release their lifelines, retire publication and
  communication authoritatively, and confirm OS exit. The deadline bounds only
  the caller's wait; an incomplete return retains ownership and cleanup keeps
  running.
- Repeated release and termination calls operate on the same opaque record; they do not
  reconstruct authority from a process id or name. A cleanup escalation cannot
  be downgraded by a later graceful release request.
- Complete release joins authoritative exact-occurrence publication and
  communication retirement with confirmed OS exit for every admitted
  occurrence. Dropping the handle does not drop Sintra's retained obligation.

Failures:

- Returns an empty handle (with a logged error) when validation or lifecycle
  admission rejects before custody acceptance.
- Resource exceptions before durable acceptance may propagate; no OS creation
  authority has been granted at that point.
- Readiness deadline expiry or resolution failure returns accepted custody with
  `readiness_reached == false`; it is not a spawn-count failure.

Example source:

- [tests/spawn_wait_test.cpp](../../tests/spawn_wait_test.cpp)
- [tests/spawn_detached_test.cpp](../../tests/spawn_detached_test.cpp)
- [tests/lifeline_basic_test.cpp](../../tests/lifeline_basic_test.cpp)
- [tests/managed_child_public_cleanup_contract_test.cpp](../../tests/managed_child_public_cleanup_contract_test.cpp)

See also:

- [sintra::join_swarm](join_swarm.md)
- [sintra::create_external_process_invitation](external_process_invitation.md)
- [sintra::Process_descriptor](process_descriptor.md)
- [sintra::init](init.md)
