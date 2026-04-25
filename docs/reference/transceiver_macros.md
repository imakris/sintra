# Transceiver macros

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

The transceiver macros declare typed messages and export member functions
for messaging or RPC. They are designed to be used inside the body of a
class that inherits through [`sintra::Derived_transceiver`](derived_transceiver.md).
Each user-facing macro generates an inheritance assertion that fails to
compile if expanded outside such a class.

User-facing macros:

```cpp
SINTRA_TYPE_ID(idv)
SINTRA_MESSAGE(name, fields...)
SINTRA_MESSAGE_EXPLICIT(name, idv, fields...)
SINTRA_RPC(method)
SINTRA_RPC_STRICT(method)
SINTRA_UNICAST(method)
```

Use when:

- `SINTRA_TYPE_ID(idv)`: pin the transceiver's own type id so the value is
  stable across builds and toolchains. `idv` must be positive and not
  exceed `sintra::max_user_type_id`.
- `SINTRA_MESSAGE(name, fields...)`: declare a message type with auto-assigned
  type id. See [Transceiver messages](transceiver_messages.md) for the body
  contract and a worked example.
- `SINTRA_MESSAGE_EXPLICIT(name, idv, fields...)`: same as above with a
  pinned id; `idv` must be positive and within `max_user_type_id`.
- `SINTRA_RPC(method)`: export a member function as a blocking and
  asynchronous RPC. Same-process blocking calls may take a direct
  shortcut. See [`guide.md`](../guide.md#rpc).
- `SINTRA_RPC_STRICT(method)`: export a member function so that local and
  remote callers always go through the transported RPC path. Required when
  the asynchronous handle surface must work for same-process targets.
  See [`guide.md`](../guide.md#rpc).
- `SINTRA_UNICAST(method)`: export a `void` member function as a
  fire-and-forget unicast. Generates `rpc_method` only; `rpc_async_method`
  is rejected at compile time. See [`guide.md`](../guide.md#rpc).

Contract:

- All user-facing macros must appear inside a class derived through
  `Derived_transceiver<T>`. The macros depend on the
  `Transceiver_type` alias being defined.
- `SINTRA_RPC*` and `SINTRA_UNICAST` exports cannot return a reference and
  cannot accept non-const reference parameters. Both restrictions are
  enforced through static assertions in the generated code.
- `SINTRA_UNICAST` requires a `void` return type. The static assertion
  fires otherwise.
- `SINTRA_TYPE_ID` must be expanded once per transceiver type. Two
  expansions in the same class will define `sintra_type_id()` twice and
  fail to compile.

Threading and lifecycle:

- Generated `rpc_method` and `rpc_async_method` static functions are
  invoked from caller threads. Their dispatch and reply paths use Sintra
  reader threads internally.
- The exporter object that owns the member function must remain alive
  while remote callers are issuing requests. Use `assign_name` plus
  named-instance lookup to publish the exporter.

Failures:

- Compile-time errors when the macro is used outside a properly derived
  transceiver, when an RPC export violates the reference rules, or when
  an explicit id is out of range.
- Run-time RPC failures (target not resolvable, target instance gone,
  remote exception) are surfaced as exceptions in the caller. See
  [`guide.md`](../guide.md#rpc).

Internal macros (not for user code):

```cpp
SINTRA_RPC_EXPLICIT(method)         // uses reserved internal type ids
SINTRA_RPC_STRICT_EXPLICIT(method)  // uses reserved internal type ids
SINTRA_MESSAGE_BASE(name, idv, ...) // implementation backbone of SINTRA_MESSAGE*
SINTRA_MESSAGE_RESERVED(name, ...)  // declares messages in the reserved id range
```

These macros expand to symbols that touch reserved id ranges and
internal structures. Application code should use `SINTRA_MESSAGE`,
`SINTRA_MESSAGE_EXPLICIT`, `SINTRA_RPC`, `SINTRA_RPC_STRICT`, or
`SINTRA_UNICAST` instead.

Example source:

- [example/sintra/sintra_example_2_rpc_append.cpp](../../example/sintra/sintra_example_2_rpc_append.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [example/sintra/sintra_example_7_explicit_type_ids.cpp](../../example/sintra/sintra_example_7_explicit_type_ids.cpp)

Reduced example (from `sintra_example_6_unicast_send_to.cpp`):

```cpp
#include <sintra/sintra.h>

struct Message_receiver : sintra::Derived_transceiver<Message_receiver>
{
    void handle_unicast(const Unicast_message& msg);
    SINTRA_UNICAST(handle_unicast)
};
```

See also:

- [Transceiver messages](transceiver_messages.md)
- [sintra::Derived_transceiver](derived_transceiver.md)
- [sintra::Transceiver](transceiver.md)
- [Message payloads](message_payloads.md)
- [Guide: RPC section](../guide.md#rpc)
- [Guide: Type IDs section](../guide.md#type-ids)
