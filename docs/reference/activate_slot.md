# sintra::activate_slot

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`activate_slot` registers a callback as a slot for incoming messages on the
current managed process. The deduced argument type of the callback selects
which message type the slot subscribes to. An optional sender filter restricts
the slot to messages emitted by a specific transceiver instance.

Signature:

```cpp
template <typename FT, typename SENDER_T = void>
auto sintra::activate_slot(
    const FT& slot_function,
    sintra::Typed_instance_id<SENDER_T> sender_id =
        sintra::Typed_instance_id<void>(sintra::any_local_or_remote));
```

The return value is a callable handler deactivator. Invoking it removes this
specific slot. The deactivator is also tracked in the managed process so that
[`sintra::deactivate_all_slots`](deactivate_all_slots.md) can release it.

Use when:

- A process needs to react to messages of a known type.
- The handler is a free function, a lambda, a functor, or a member function
  on a transceiver.
- Filtering by sender is needed to ignore messages produced by other
  transceivers.

Contract:

- The callback's single argument selects the message type. Two argument
  shapes are supported:
  - A non-`Message_prefix` argument means "value-style slot": the value is
    the body wrapped by [`sintra::world`](world.md), [`sintra::local`](local.md),
    or [`sintra::remote`](remote.md). For example a slot taking
    `int` receives values sent via `world() << 1`.
  - An argument deriving from `Message_prefix` (a typed message declared via
    `SINTRA_MESSAGE` or `SINTRA_MESSAGE_EXPLICIT`) means "message-style slot":
    the slot receives the full message including its variable-buffer fields.
- The default `sender_id` accepts any local or remote sender. Pass a
  `Typed_instance_id<T>` to restrict the slot to a specific instance.
- The optional second template parameter `SENDER_T` is normally deduced from
  `sender_id`.
- The returned deactivator may be invoked at most once. Subsequent calls are
  no-ops.

Threading and lifecycle:

- Slot handlers run on a Sintra-owned reader thread. Protect any state
  shared with application threads using normal C++ synchronization
  primitives.
- Do not call [`sintra::receive`](receive.md) from a slot handler; that
  would deadlock the very thread that delivers the awaited message.
- Slots are scoped to the current managed process. They are released by the
  returned deactivator, by `deactivate_all_slots()`, or implicitly during
  process teardown.

Failures:

- Compile-time error when the callback does not have a single deducible
  argument or when the argument type is not a supported message body or
  message type.
- Activation fails to deliver if `sintra::init()` has not been called yet.

Example source:

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [example/sintra/sintra_example_7_explicit_type_ids.cpp](../../example/sintra/sintra_example_7_explicit_type_ids.cpp)
- [tests/receive_test.cpp](../../tests/receive_test.cpp)

Reduced example (from `sintra_example_0_basic_pubsub.cpp`):

```cpp
#include <sintra/sintra.h>

int process_2()
{
    auto string_slot = [](const std::string& str) {
        sintra::console() << "received \"" << str << "\"\n";
    };

    sintra::activate_slot(string_slot);

    sintra::barrier("1st barrier");
    sintra::barrier("2nd barrier");
    return 0;
}
```

See also:

- [sintra::deactivate_all_slots](deactivate_all_slots.md)
- [sintra::receive](receive.md)
- [sintra::world](world.md)
- [sintra::Derived_transceiver](derived_transceiver.md)
- [Transceiver messages](transceiver_messages.md)
