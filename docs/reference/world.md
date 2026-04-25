# sintra::world

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`world()` returns the maildrop bound to all recipients (local plus remote). It
is the streaming entry point for broadcasting plain values from the current
managed process. Each `<<` operation wraps the value in a `Message<Enclosure<T>>`
and sends it as the managed-process transceiver.

Signature:

```cpp
sintra::Maildrop<sintra::any_local_or_remote>& sintra::world();
```

Use when:

- Broadcasting a plain value (string, integer, struct, container) to every
  managed process in the swarm, including the current one.
- Sending an empty signal type such as `struct Stop {}` to all processes.
- The wrapper message type does not need to be named; only the value type
  matters at the receiving end.

Contract:

- Values must be either trivial standard-layout types, types convertible to
  `sintra::variable_buffer`, or one of the streaming overloads provided by
  `Maildrop` (string literal, C string, fixed-size array).
- The sender identity carried in the message is the managed-process
  transceiver, not the calling user transceiver. To preserve a user
  transceiver as sender, use the `emit_*` methods on
  [`sintra::Derived_transceiver`](derived_transceiver.md) instead.
- Operator chaining is supported and equivalent to repeated single sends.

Threading and lifecycle:

- Requires a successfully initialized Sintra runtime. Call `sintra::init()`
  before sending through `world()`, and stop using the maildrop before runtime
  teardown.
- Slots that handle the broadcast run on Sintra reader threads. Use a
  [`barrier`](../guide.md#barriers-and-fences) when a sender must
  wait until receivers have activated their slots, and a `processing_fence_t`
  barrier when later code must observe handler side effects.

Failures:

- Compile-time error if the value type is not supported by the message
  serialiser. See [`message_payloads.md`](message_payloads.md) for the type
  contract.

Example source:

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [tests/basic_pub_sub.cpp](../../tests/basic_pub_sub.cpp)

Reduced example (from `sintra_example_0_basic_pubsub.cpp`):

```cpp
#include <sintra/sintra.h>

int process_1()
{
    sintra::barrier("1st barrier");
    sintra::world() << "good morning";
    sintra::world() << 1;
    sintra::world() << "good afternoon" << "good evening" << "good night";
    sintra::barrier("2nd barrier");
    return 0;
}
```

See also:

- [sintra::local](local.md)
- [sintra::remote](remote.md)
- [sintra::Maildrop](maildrop.md)
- [sintra::activate_slot](activate_slot.md)
- [sintra::Derived_transceiver](derived_transceiver.md)
