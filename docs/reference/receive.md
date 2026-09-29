# sintra::receive

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
template <typename MESSAGE_T>
MESSAGE_T sintra::receive();

template <typename MESSAGE_T, typename SENDER_T>
MESSAGE_T sintra::receive(sintra::Typed_instance_id<SENDER_T> sender_id);

template <typename MESSAGE_T>
sintra::Owned_message<MESSAGE_T> sintra::receive_owned();

template <typename MESSAGE_T, typename SENDER_T>
sintra::Owned_message<MESSAGE_T> sintra::receive_owned(
    sintra::Typed_instance_id<SENDER_T> sender_id);
```

Description: Block the calling control thread until a message with the
requested body type arrives, then return the message value. The
sender-filtered overload accepts only messages emitted from a specific
transceiver instance.

## Parameters

- `MESSAGE_T` (template parameter) — the body type accepted by the slot.
  For values broadcast through [`sintra::world`](world.md),
  [`sintra::local`](local.md), or [`sintra::remote`](remote.md), this is
  the value type itself (for example `int` or `std::string`). For typed
  messages declared via `SINTRA_MESSAGE` it is the nested message type.
- `sender_id` (filtered overload) — the transceiver whose messages this
  receive call accepts. Other senders are ignored.

## Returns

- A value of type `MESSAGE_T`, holding the body of the first matching
  message.
- `receive_owned` returns `Owned_message<MESSAGE_T>`, a `std::unique_ptr`
  with a deleter for the complete aligned message allocation. It supports
  generated `sintra::Message` types with
  trivially copyable bodies and `void` return types, including messages with
  `message_string` and `typed_variable_buffer` fields. Ordinary value types
  such as `std::string` use `receive`, not `receive_owned`.

## Throws

- Does not throw on a normal completion.
- `receive_owned` propagates allocation or frame-copy errors to its calling
  thread after removing the temporary slot.
- Calling from a request-reader thread is a programmer error. In debug
  builds the function aborts with a diagnostic; in release builds the
  call deadlocks.

## Use when

- A top-level process function needs to wait synchronously for a single
  message before continuing.
- Bootstrapping handshakes such as exchanging instance ids before the
  main loop begins.
- Coordinating teardown by waiting for an explicit `Stop`-style signal.

## Contract

- The function activates a temporary slot internally, waits on a
  condition variable, then deactivates that slot before returning. Only
  one matching message is consumed per call.
- The sender-filtered overload only returns when the message is
  produced by the transceiver wrapped in `sender_id`.
- `receive` is not a handler API. Must not be called from inside another
  slot or RPC callback; the awaited message would have to be dispatched
  by the very thread that is blocked.

## Threading and lifecycle

- Must be called from a control thread (a process entry function or a
  thread that is not a Sintra reader thread).
- For generated `SINTRA_MESSAGE` types, `receive` requires supported fixed
  fields. Plain fields (including array elements and nested plain structs)
  must be trivial and standard-layout; `Resolvable_instance_id` is also
  supported. Initializers on fixed fields do not change this classification.
  Variable-buffer fields fail to compile with a diagnostic directing callers
  to `receive_owned<T>()`. The same check applies to both receive overloads.
  Ordinary owning values such as `std::string` and `std::vector<int>` continue
  to use `receive<T>()`.
- `receive_owned` retains the fixed object and variable payload together in
  an independent allocation. Move the smart pointer to transfer ownership;
  the message stays at the same address. Its descriptor fields remain valid
  until that owner is reset or destroyed, including across further receives
  and runtime shutdown. Copying or moving `*message` by value does not transfer
  the trailing payload. Convert a field to its ordinary owning container if
  it must outlive the message owner.
- The internal slot is removed automatically before the function
  returns. No deactivation by the caller is required.

## Notes

- The reader copies the complete frame into reusable dispatch storage before
  invoking matching handlers. A receive slot is one such handler; its result
  must have an independent lifetime. Local copies/deserialization from that
  storage are acceptable and do not extend ring copy protection. Retaining
  this shared dispatch buffer is a [settled design decision](../design_principles.md#retaining-the-shared-dispatch-buffer),
  including for fixed-size payloads. `receive_owned` makes a local owning copy
  from that dispatch frame; it does not change the reader or ring protocol.

- Apply timeouts externally by signalling from another thread that
  emits the awaited message type. `receive` does not expose a deadline
  parameter.

## Example

```cpp
#include <sintra/sintra.h>

int receiver_process(sintra::instance_id_type sender_a_id)
{
    auto msg = sintra::receive<DataMessage>(
        sintra::Typed_instance_id<sintra::Managed_process>(sender_a_id));
    // use msg.value, msg.score
    return 0;
}
```

For a generated message with a variable field:

```cpp
struct Status_bus : sintra::Derived_transceiver<Status_bus>
{
    SINTRA_MESSAGE(Status, sintra::message_string text);
};

auto message = sintra::receive_owned<Status_bus::Status>();
std::string text = message->text;
auto retained = std::move(message); // frame address and field offsets stay valid
```

## Example source

- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [tests/receive_test.cpp](../../tests/receive_test.cpp)

## See also

- [`sintra::activate_slot`](activate_slot.md)
- [`sintra::deactivate_all_slots`](deactivate_all_slots.md)
- [`sintra::world`](world.md)
- [`sintra::Typed_instance_id`](typed_instance_id.md)
- [Message payloads](message_payloads.md)
