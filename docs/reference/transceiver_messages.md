# Transceiver messages

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

Transceiver messages are message types nested inside a
[`sintra::Derived_transceiver`](derived_transceiver.md) and declared with
`SINTRA_MESSAGE` or `SINTRA_MESSAGE_EXPLICIT`. Each declaration generates a
plain-data body struct and an alias to `sintra::Message<Body, void, ID, Owner>`
that carries the body across the messaging layer.

Signature (relevant template):

```cpp
template <
    typename T,
    typename RT = void,
    sintra::type_id_type ID = 0,
    typename EXPORTER = void
>
struct Message : public sintra::Message_prefix, public T
{
    using body_type     = T;
    using return_type   = RT;
    using exporter      = EXPORTER;

    static sintra::type_id_type id();    // resolved type id
    static constexpr sintra::type_id_type sintra_type_id();
};
```

Declaration macros:

```cpp
SINTRA_MESSAGE(name, fields...);
SINTRA_MESSAGE_EXPLICIT(name, idv, fields...);
```

`fields...` accepts up to sixteen field declarations of the form
`type identifier`, separated by commas. The macros generate a `name` alias
to a `sintra::Message<...>` whose body is a struct with those fields.

Use when:

- A transceiver wants to broadcast or unicast typed payloads with named
  fields (richer than the auto-wrapped values used by
  [`sintra::Maildrop`](maildrop.md)).
- Slots must filter by message type rather than by raw value type.
- The same payload must be addressable as `Outer::name` from any
  translation unit that sees the transceiver definition.
- A pinned message id is required across mixed toolchains; in that case
  use `SINTRA_MESSAGE_EXPLICIT(name, idv, ...)` and follow the
  [Type IDs section](../guide.md#type-ids) of the guide.

Contract:

- `SINTRA_MESSAGE` declares the message inside the `Transceiver_type`
  body. It must be used in a class that inherits through
  `Derived_transceiver<T>`; the inheritance assertion fails to compile
  otherwise.
- Field declarations are stored exactly as written in the generated body.
  Use trivial standard-layout fixed-size fields, Sintra message elements, or
  explicit variable-buffer field wrappers such as `sintra::message_string` and
  `sintra::typed_variable_buffer<T>`. Top-level maildrop values and RPC
  arguments can transform `std::string`/`std::vector<T>` for you, but
  `SINTRA_MESSAGE` fields should name the transported field type directly.
  See [`message_payloads.md`](message_payloads.md).
- The generated alias is named exactly as the macro argument (`Outer::name`).
  Both the body fields and the `Message_prefix` fields are reachable on the
  same object; field names declared by the macro must not clash with
  prefix names such as `bytes_to_next_message` or `function_instance_id`.
- The message is sent through one of the
  [`sintra::Derived_transceiver`](derived_transceiver.md) emitters
  (`emit_local`, `emit_remote`, `emit_global`).

Threading and lifecycle:

- Messages are constructed in-place inside the request ring of the
  sending process. Slot handlers receive them by reference on a Sintra
  reader thread.
- Variable-buffer fields (`message_string`, `typed_variable_buffer<T>`)
  reference ring memory. Copy them out before the handler returns or the
  receive scope ends if they must outlive that point.

Failures:

- Compile-time error if the macro is used outside a transceiver derived
  through `Derived_transceiver<T>`.
- Compile-time error if a field type is not supported by the serialiser.
- `SINTRA_MESSAGE_EXPLICIT` enforces `idv > 0` and
  `idv <= sintra::max_user_type_id` at compile time.

Example source:

- [example/sintra/sintra_example_7_explicit_type_ids.cpp](../../example/sintra/sintra_example_7_explicit_type_ids.cpp)

Reduced example (from `sintra_example_7_explicit_type_ids.cpp`):

```cpp
#include <sintra/sintra.h>

constexpr sintra::type_id_type k_bus_id  = 0x120;
constexpr sintra::type_id_type k_ping_id = 0x121;

struct Explicit_bus : sintra::Derived_transceiver<Explicit_bus>
{
    SINTRA_TYPE_ID(k_bus_id)
    SINTRA_MESSAGE_EXPLICIT(ping, k_ping_id, int value)
};

void demo(Explicit_bus& bus)
{
    sintra::activate_slot([](const Explicit_bus::ping& msg) {
        sintra::console() << "ping value=" << msg.value << '\n';
    });
    bus.emit_global<Explicit_bus::ping>(7);
}
```

See also:

- [Transceiver macros](transceiver_macros.md)
- [Message payloads](message_payloads.md)
- [sintra::Derived_transceiver](derived_transceiver.md)
- [sintra::activate_slot](activate_slot.md)
