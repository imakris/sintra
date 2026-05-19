# sintra::Transceiver

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`Transceiver` is the runtime object that owns instance identity, slot
registrations, and message dispatch wiring. User transceivers do not derive
from `Transceiver` directly; they go through
[`sintra::Derived_transceiver`](derived_transceiver.md), which adds typed
emission helpers and the RPC plumbing required by the
[transceiver macros](transceiver_macros.md).

Signature (relevant public members):

```cpp
struct Transceiver
{
    using Transceiver_type = Transceiver;

    template <typename = void>
    Transceiver(const std::string& name = "", uint64_t id = 0);
    template <typename = void>
    Transceiver(const char* name, uint64_t id = 0);

    ~Transceiver();

    sintra::instance_id_type instance_id();

    template <typename = void>
    bool assign_name(const std::string& name);

    template <typename = void>
    void destroy();

    template <typename = void>
    void deactivate_all();
};
```

Use when:

- Inspecting a transceiver's runtime identity (`instance_id()`).
- Publishing a transceiver under a swarm-wide name (`assign_name`).
- Releasing all handlers registered through this transceiver
  (`deactivate_all`).
- Performing an explicit teardown of a long-lived transceiver before its
  destructor runs (`destroy`).

Contract:

- `instance_id()` returns the transceiver's process-scoped instance id.
  The high bits identify the owning process, the low bits identify the
  transceiver within that process.
- `assign_name(name)` publishes the transceiver under `name`, making it
  resolvable by other processes through `Resolvable_instance_id`-style
  lookups (used by RPC). It returns `true` on success, `false` when the
  coordinator rejects the publication.
- `destroy()` deactivates this transceiver's slots, unpublishes the name
  if any was assigned, and removes the instance pointer from the local
  registry. After it returns, the instance id is no longer usable.
- `deactivate_all()` walks the deactivator list and releases every slot
  registered through this transceiver in reverse order. It does not
  unpublish the transceiver and does not free the underlying object.
- The constructor accepting a `name` calls `assign_name(name)` after
  identity setup. Pass an empty name to skip publication.

Threading and lifecycle:

- Slot handlers run on Sintra reader threads. Application code must
  synchronise external state explicitly.
- `destroy()` is safe to call when the runtime is still alive. During raw
  teardown the destructor short-circuits if the messaging layer has
  already stopped, since RPC would otherwise deadlock.
- A transceiver must outlive every callback that captures it.

Failures:

- `assign_name` returns `false` when the name is empty, the coordinator
  already has a publication for the given name, the transceiver was already
  published, or the per-process publication limit has been reached.
- `destroy` swallows RPC failures during shutdown so it can be invoked
  from teardown paths without throwing.

Example source:

- [example/sintra/sintra_example_2_rpc_append.cpp](../../example/sintra/sintra_example_2_rpc_append.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)

Reduced example (from `sintra_example_2_rpc_append.cpp`):

```cpp
#include <sintra/sintra.h>

struct Remotely_accessible : sintra::Derived_transceiver<Remotely_accessible>
{
    std::string append(const std::string& s, int v) { return std::to_string(v) + ": " + s; }
    SINTRA_RPC(append)
};

int process_1()
{
    Remotely_accessible ra;
    ra.assign_name("instance name");
    sintra::barrier("1st barrier");
    sintra::barrier("2nd barrier");
    return 0;
}
```

See also:

- [sintra::Derived_transceiver](derived_transceiver.md)
- [sintra::activate_slot](activate_slot.md)
- [sintra::deactivate_all_slots](deactivate_all_slots.md)
- [Transceiver macros](transceiver_macros.md)
- [Transceiver messages](transceiver_messages.md)
