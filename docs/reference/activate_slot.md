# sintra::activate_slot

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
template <typename FT, typename SENDER_T = void>
auto sintra::activate_slot(
    const FT& slot_function,
    sintra::Typed_instance_id<SENDER_T> sender_id =
        sintra::Typed_instance_id<void>(sintra::any_local_or_remote));
```

Description: Register a callback as a slot for incoming messages on the
current managed process. The deduced argument type of the callback
selects which message type the slot subscribes to. An optional sender
filter restricts the slot to messages emitted by a specific transceiver
instance.

## Parameters

- `slot_function` — a callable with a single deducible argument. The
  argument type selects the message type. Free functions, lambdas,
  functors, and member function references are all accepted.
- `sender_id` — restricts the slot to messages emitted by a specific
  transceiver. Defaults to a wildcard accepting any local or remote
  sender.
- `SENDER_T` (template parameter) — the transceiver type expected as the
  sender. Normally deduced from `sender_id`.

## Returns

- A callable handler deactivator. Invoking it removes this specific
  slot. The deactivator is single-use and remains valid only while the
  slot is still active and the owning runtime/transceiver is alive.
  The deactivator is also tracked in the managed process so that
  [`sintra::deactivate_all_slots`](deactivate_all_slots.md) can release it.

## Throws

- Compile-time error when the callback does not have a single deducible
  argument or the argument type is not a supported message body or
  message type.
- `std::runtime_error` when called without an active Sintra runtime, for
  example before [`sintra::init`](init.md) or after teardown through
  [`sintra::shutdown`](shutdown.md) or [`sintra::leave`](leave.md).

## Use when

- A process needs to react to messages of a known type.
- The handler is a free function, a lambda, a functor, or a member
  function on a transceiver.
- Filtering by sender is needed to ignore messages produced by other
  transceivers.

## Contract

- Two callback argument shapes are supported:
  - A non-`Message_prefix` argument means *value-style slot*: the value
    is the body wrapped by [`sintra::world`](world.md), [`sintra::local`](local.md),
    or [`sintra::remote`](remote.md). For example a slot taking `int`
    receives values sent via `world() << 1`.
  - An argument deriving from `Message_prefix` (a typed message declared
    via `SINTRA_MESSAGE` or `SINTRA_MESSAGE_EXPLICIT`) means
    *message-style slot*: the slot receives the full message including
    its variable-buffer fields.
- The returned deactivator may be invoked at most once. Subsequent calls
  are outside the API contract. Do not invoke a returned deactivator after
  the slot has already been released by
  [`sintra::deactivate_all_slots`](deactivate_all_slots.md) or teardown.

## Threading and lifecycle

- Slot handlers run on a Sintra-owned reader thread. Protect any state
  shared with application threads using normal C++ synchronisation
  primitives.
- `slot_function` must not call [`sintra::receive`](receive.md); that
  would deadlock the very thread that delivers the awaited message.
- Slots are scoped to the current managed process. They are released by
  the returned deactivator, by
  [`sintra::deactivate_all_slots`](deactivate_all_slots.md), or
  implicitly during process teardown.
- When invoked from a control thread, the deactivator waits for any
  in-flight slot dispatch that already copied the handler to finish
  before returning. When invoked from inside a slot handler, it removes
  the slot for future dispatch but does not wait for the current handler
  stack to unwind.

## Example

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

## Example source

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [example/sintra/sintra_example_7_explicit_type_ids.cpp](../../example/sintra/sintra_example_7_explicit_type_ids.cpp)
- [tests/receive_test.cpp](../../tests/receive_test.cpp)

## See also

- [`sintra::deactivate_all_slots`](deactivate_all_slots.md)
- [`sintra::receive`](receive.md)
- [`sintra::world`](world.md)
- [`sintra::Derived_transceiver`](derived_transceiver.md)
- [Transceiver messages](transceiver_messages.md)
