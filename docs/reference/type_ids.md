# type_id_type and related helpers

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`type_id_type` is the 64-bit cross-process type identifier carried by every
Sintra message. The runtime auto-assigns ids for transceiver and message
types, and a small set of constants and helpers describe the user-id range
that can be pinned with the explicit-id macros.

Signature:
```cpp
namespace sintra {

using type_id_type = uint64_t;

constexpr type_id_type invalid_type_id;     // 0
constexpr type_id_type max_user_type_id;    // largest value SINTRA_TYPE_ID accepts
constexpr type_id_type user_type_id_flag;   // high-bit tag for user ids

constexpr bool         is_user_type_id(type_id_type v);
constexpr type_id_type make_user_type_id(type_id_type v);

} // namespace sintra
```

Use when:
- Declaring a stable id range (for example, in a header shared across multiple
  toolchains) and reserving ids inside the user range.
- Inspecting a `type_id_type` to decide whether it was produced by a
  user-pinned id macro versus an auto-assigned id.

Contract:
- `invalid_type_id` is `0` and is the default for unset `type_id_type` fields
  in messages prior to construction.
- `user_type_id_flag` is a single high bit that tags user-pinned ids. The
  runtime guarantees that auto-assigned ids never set this bit.
- `max_user_type_id` is the largest value that may be supplied to
  `SINTRA_TYPE_ID(idv)` or `SINTRA_MESSAGE_EXPLICIT(name, idv, ...)`. The
  generated id is `make_user_type_id(idv)`.
- `make_user_type_id(v)` sets the user-id flag on `v`. It is constexpr and
  never throws; supplying a value greater than `max_user_type_id` is rejected
  at compile time by the explicit-id macros that use it.
- `is_user_type_id(v)` returns `true` when `v` carries the user flag and is
  not the reserved sentinel `~type_id_type(0)`.

Threading and lifecycle:
- All listed helpers are pure value-level operations. They are safe to call
  from any thread and do not depend on the Sintra runtime being initialised.

Failures:
- None. The helpers do not throw.

Example source:
- [tests/explicit_type_id_test.cpp](../../tests/explicit_type_id_test.cpp)
- [example/sintra/sintra_example_7_explicit_type_ids.cpp](../../example/sintra/sintra_example_7_explicit_type_ids.cpp)

See also:
- [SINTRA_TYPE_ID and SINTRA_MESSAGE_EXPLICIT](explicit_type_ids.md)
- [instance ids](instance_ids.md)
