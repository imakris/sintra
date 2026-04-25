# sintra::Derived_transceiver

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`Derived_transceiver<Derived_T, Parent>` is the CRTP base for user
transceivers. It composes the [`sintra::Transceiver`](transceiver.md) runtime
state with typed emission helpers (`emit_local`, `emit_remote`, `emit_global`)
and the plumbing required by [transceiver macros](transceiver_macros.md)
that declare messages or export RPC endpoints.

Signature:

```cpp
template <typename Derived_T, typename Parent = void>
struct Derived_transceiver : Parent
{
    using Transceiver_type = Derived_T;

    template <typename MESSAGE_T,
              typename SENDER_T = Transceiver_type,
              typename... Args>
    void emit_local(Args&&... args);

    template <typename MESSAGE_T,
              typename SENDER_T = Transceiver_type,
              typename... Args>
    void emit_remote(Args&&... args);

    template <typename MESSAGE_T,
              typename SENDER_T = Transceiver_type,
              typename... Args>
    void emit_global(Args&&... args);

    static sintra::Named_instance<Transceiver_type>
        named_instance(const std::string& name);
};

// First-level CRTP: Derived_transceiver<T> derives from sintra::Transceiver.
// Higher-level inheritance: Derived_transceiver<Child, Parent> derives from Parent,
// which itself must already derive from sintra::Transceiver.
```

Use when:

- Defining a user transceiver type `T` that exposes typed messages or RPC
  endpoints (`struct T : sintra::Derived_transceiver<T> { ... };`).
- Layering a child transceiver type on top of an existing transceiver
  (`struct Child : sintra::Derived_transceiver<Child, Parent> { ... };`).
- Sending typed messages whose sender identity must be the transceiver
  itself rather than the managed-process transceiver.
- Looking up a peer instance by name through `T::named_instance("...")`.

Contract:

- `Transceiver_type` aliases the deriving type, which the transceiver
  macros use in their internal assertions and exports.
- `emit_local` sends to handlers in the current process, `emit_remote` to
  handlers in other processes, and `emit_global` to both. The sender
  carried in the message is the transceiver instance.
- The default `SENDER_T` template parameter is the deriving transceiver
  type. Override it explicitly when the message must be reported as
  originating from a different transceiver type.
- `MESSAGE_T` is normally a message type produced by `SINTRA_MESSAGE` or
  `SINTRA_MESSAGE_EXPLICIT` nested in the transceiver. Plain
  `Message<Enclosure<T>>` types declared elsewhere are also accepted.
- `named_instance(name)` returns a `sintra::Named_instance<T>` that can be
  passed where typed instance lookup is expected (for example as a slot
  filter that resolves the sender by name).

Threading and lifecycle:

- Each `emit_*` call drops the message into the current process's request
  ring and returns. Delivery and dispatch are asynchronous.
- The standard transceiver lifecycle rules apply: the transceiver instance
  must outlive any handler it owns, and `destroy()` should be called for
  long-lived transceivers before teardown if the application wants to
  unpublish them deterministically.

Failures:

- Compile-time error if the message type is not derived from
  `Message_prefix`.
- Compile-time error if the sender type does not match the message's
  declared exporter.
- Sending requires a successfully initialized Sintra runtime. Call
  `sintra::init()` before emitting messages, and stop emitting before runtime
  teardown.

Example source:

- [example/sintra/sintra_example_2_rpc_append.cpp](../../example/sintra/sintra_example_2_rpc_append.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [example/sintra/sintra_example_7_explicit_type_ids.cpp](../../example/sintra/sintra_example_7_explicit_type_ids.cpp)

Reduced example (from `sintra_example_7_explicit_type_ids.cpp`):

```cpp
#include <sintra/sintra.h>

struct Explicit_bus : sintra::Derived_transceiver<Explicit_bus>
{
    SINTRA_TYPE_ID(k_bus_id)
    SINTRA_MESSAGE_EXPLICIT(ping, k_ping_id, int value)
};

void demo()
{
    Explicit_bus bus;
    sintra::activate_slot([](const Explicit_bus::ping& msg) {
        sintra::console() << "ping value=" << msg.value << '\n';
    });
    bus.emit_global<Explicit_bus::ping>(7);
}
```

See also:

- [sintra::Transceiver](transceiver.md)
- [Transceiver messages](transceiver_messages.md)
- [Transceiver macros](transceiver_macros.md)
- [sintra::activate_slot](activate_slot.md)
- [sintra::world](world.md)
