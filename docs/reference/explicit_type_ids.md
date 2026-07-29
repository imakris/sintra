# SINTRA_TYPE_ID and SINTRA_MESSAGE_EXPLICIT

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`SINTRA_TYPE_ID(idv)` and `SINTRA_MESSAGE_EXPLICIT(name, idv, fields...)` are
the two macros for pinning a transceiver type id and a message type id to a
user-chosen value. They emit the same `type_id_type` on every build that uses
the same `idv`, which makes identity deterministic across ABI-compatible
builds.

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
- Multiple ABI-compatible builds would otherwise derive different
  auto-assigned ids.
- A protocol needs deterministic ids for serialisation metadata, logging, or
  cross-build identity within an ABI-compatible application.

Contract:
- `SINTRA_TYPE_ID(idv)` defines a static `sintra_type_id()` member returning
  `make_user_type_id(idv)`. Sintra reads this when assigning the transceiver's
  type id, so the same `idv` produces the same `type_id_type` everywhere.
- `SINTRA_MESSAGE_EXPLICIT(name, idv, fields...)` declares a message type with
  body fields `fields...` and pins its id to `make_user_type_id(idv)`. It
  requires the surrounding type to be a `Derived_transceiver`, since the macro
  emits a static assertion that ties the message to the enclosing
  `Transceiver_type`.
- Explicit ids stabilize message and transceiver identity only. They do not
  stabilize raw C++ object representation, including size, alignment, or
  padding.
- Every joining process must present exactly the coordinator's startup ABI
  token. That token contains compiler, standard-library, platform, and
  architecture identities plus the Sintra ring ABI version.
- A token mismatch, including between MSVC and MinGW builds, is rejected
  before that joining process opens its request/reply message rings. A
  matching token is required, but it does not guarantee that raw C++ object
  representations are interoperable; explicit ids provide no such guarantee
  either.
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
