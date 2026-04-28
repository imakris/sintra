# sintra::world

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
sintra::Maildrop<sintra::any_local_or_remote>& sintra::world();
```

Description: Return the maildrop bound to all recipients (local plus
remote). Use it to broadcast plain values from the current managed
process to every handler in the swarm. Each `<<` operation wraps the
value in `Message<Enclosure<T>>` and emits it as the managed-process
transceiver. Companion functions [`sintra::local`](local.md) and
[`sintra::remote`](remote.md) restrict delivery to the same locality
classes.

## Returns

- A reference to the process-wide `Maildrop<any_local_or_remote>` static.
  See [`sintra::Maildrop`](maildrop.md) for the streaming API.

## Throws

- Compile-time error when the value type does not satisfy the message
  serialiser contract. See [Message payloads](message_payloads.md).

## Use when

- Broadcasting a plain value (string, integer, struct, container) to
  every managed process in the swarm, including the current one.
- Sending an empty signal type such as `struct Stop {}` to all
  processes.
- The wrapper message type does not need to be named; only the value
  type matters at the receiving end.

## Contract

- The sender carried in the message is the managed-process transceiver,
  not the calling user transceiver. Use the `emit_*` methods on
  [`sintra::Derived_transceiver`](derived_transceiver.md) when the
  originating user transceiver must be the message sender.
- Operator chaining is supported and equivalent to repeated single
  sends.
- Element types must satisfy the contract described in
  [Message payloads](message_payloads.md).

## Threading and lifecycle

- The accessor returns a reference to a function-local static; calling
  it is thread-safe.
- Sending requires a successfully initialised Sintra runtime. Call
  [`sintra::init`](init.md) before sending through `world`, and stop
  using the maildrop before runtime teardown.
- Slots that handle the broadcast run on Sintra reader threads. Use
  [`sintra::barrier`](barrier.md) when a sender must wait until receivers
  have activated their slots, and [`processing_fence_t`](barrier_modes.md)
  when later code must observe handler side effects.

## Example source

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [tests/basic_pub_sub.cpp](../../tests/basic_pub_sub.cpp)

## See also

- [`sintra::local`](local.md)
- [`sintra::remote`](remote.md)
- [`sintra::Maildrop`](maildrop.md)
- [`sintra::activate_slot`](activate_slot.md)
- [`sintra::Derived_transceiver`](derived_transceiver.md)
- [Message payloads](message_payloads.md)
