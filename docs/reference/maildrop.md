# sintra::Maildrop

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`Maildrop<LOCALITY>` is the streaming type returned by [`sintra::world`](world.md),
[`sintra::local`](local.md), and [`sintra::remote`](remote.md). It exposes
`operator<<` overloads that wrap a value in `Message<Enclosure<T>>` and emit it
from the managed-process transceiver to the recipients selected by the
locality template parameter.

Signature:

```cpp
template <sintra::instance_id_type LOCALITY>
struct Maildrop
{
    template <std::size_t N>
    Maildrop& operator<<(const char (&value)[N]);  // string literal -> std::string

    Maildrop& operator<<(const char* value);       // C string  -> std::string

    template <std::size_t N, typename T>
    Maildrop& operator<<(const T (&values)[N]);    // fixed array -> std::vector<T>

    template <typename T>
    Maildrop& operator<<(const T& value);          // generic value -> Message<Enclosure<T>>
};
```

The recommended way to obtain a `Maildrop` is through the three accessor
functions:

```cpp
sintra::Maildrop<sintra::any_local>&           sintra::local();
sintra::Maildrop<sintra::any_remote>&          sintra::remote();
sintra::Maildrop<sintra::any_local_or_remote>& sintra::world();
```

Use when:

- Broadcasting a plain C++ value rather than a typed protocol message.
- Forwarding a string literal, raw C string, or fixed-size array as a
  `std::string` or `std::vector<T>` payload without manual conversion.
- Sending an empty signal type (struct with no fields) to indicate a state
  transition.

Contract:

- The sender carried in the resulting message is always the managed-process
  transceiver. Use [`sintra::Derived_transceiver`](derived_transceiver.md)
  `emit_local`, `emit_remote`, or `emit_global` when the originating user
  transceiver must be the message sender.
- Operator chaining (`world() << a << b << c`) is supported and produces one
  message per `<<` operation.
- Element types must satisfy the message serialiser contract described in
  [`message_payloads.md`](message_payloads.md).

Threading and lifecycle:

- The accessors return references to function-local statics; calling them is
  thread-safe.
- Sending requires a successfully initialized Sintra runtime. Call
  `sintra::init()` before using a `Maildrop`, and stop sending before runtime
  teardown.

Failures:

- Compile-time error when the value type is not a trivial standard-layout
  type, an `Sintra_message_element`, or a `variable_buffer`-compatible type.

Example source:

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [tests/basic_pub_sub.cpp](../../tests/basic_pub_sub.cpp)

See also:

- [sintra::world](world.md)
- [sintra::local](local.md)
- [sintra::remote](remote.md)
- [sintra::Derived_transceiver](derived_transceiver.md)
- [Message payloads](message_payloads.md)
