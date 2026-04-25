# SINTRA_TYPE_ID and SINTRA_MESSAGE_EXPLICIT

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`SINTRA_TYPE_ID(idv)` and `SINTRA_MESSAGE_EXPLICIT(name, idv, fields...)` are
the two macros for pinning a transceiver type id and a message type id to a
user-chosen value. They emit the same `type_id_type` on every build that uses
the same `idv`, which is useful when participating processes are built with
different toolchains or otherwise need deterministic ids.

Signature:
```cpp
// Inside the body of a struct deriving from sintra::Derived_transceiver<T>:
SINTRA_TYPE_ID(idv)

// Anywhere SINTRA_MESSAGE would be used inside such a transceiver body:
SINTRA_MESSAGE_EXPLICIT(message_name, idv, /* member declarations */)
```

Both macros require:
- `(idv) > 0`
- `(idv) <= sintra::max_user_type_id`

The generated id is `sintra::make_user_type_id(idv)` in both cases.

Use when:
- Multiple processes in a swarm are built with different compilers or build
  configurations and the auto-assigned ids would otherwise diverge.
- A protocol needs deterministic ids for serialisation, logging, or
  cross-build compatibility within the application.

Contract:
- `SINTRA_TYPE_ID(idv)` defines a static `sintra_type_id()` member returning
  `make_user_type_id(idv)`. Sintra reads this when assigning the transceiver's
  type id, so the same `idv` produces the same `type_id_type` everywhere.
- `SINTRA_MESSAGE_EXPLICIT(name, idv, fields...)` declares a message type with
  body fields `fields...` and pins its id to `make_user_type_id(idv)`. It
  requires the surrounding type to be a `Derived_transceiver`, since the macro
  emits a static assertion that ties the message to the enclosing
  `Transceiver_type`.
- The application chooses unique values within its own codebase. The macros
  do not coordinate values for you.
- `is_user_type_id(...)` returns `true` for ids produced by either macro.
- The user-id range is reserved for application use only. The runtime's own
  reserved ids occupy a disjoint range and must not be referenced from
  application code; the explicit-RPC variants used internally by the
  coordinator are not part of the user-facing surface.

Threading and lifecycle:
- The macros are evaluated at compile time. Type ids materialise on first use
  through static initialisation in the message-id accessor.

Failures:
- Compile-time errors when `idv` is `0` or greater than `max_user_type_id`.
- Compile-time error when `SINTRA_MESSAGE_EXPLICIT` is placed in a type that
  is not a `Derived_transceiver` (the static assertion ties the message to
  `Transceiver_type`).

Example source:
- [example/sintra/sintra_example_7_explicit_type_ids.cpp](../../example/sintra/sintra_example_7_explicit_type_ids.cpp)
- [tests/explicit_type_id_test.cpp](../../tests/explicit_type_id_test.cpp)

See also:
- [type_id_type and helpers](type_ids.md)
