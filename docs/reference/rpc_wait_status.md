# Rpc_wait_status

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`Rpc_wait_status` is the result of a bounded wait on an `Rpc_handle<R>`. It
reports whether the wait observed a terminal state before the requested
deadline.

Signature:
```cpp
namespace sintra {

enum class Rpc_wait_status
{
    completed,
    deadline_exceeded,
};

} // namespace sintra
```

Use when:
- Calling `Rpc_handle<R>::wait_until(deadline)` and deciding what to do next
  based on whether the call reached a terminal outcome.

Contract:
- `completed` means the handle reached a terminal state at or before the
  deadline. The terminal state can then be inspected with
  `Rpc_handle<R>::state()` and the value retrieved (or the failure rethrown)
  with `Rpc_handle<R>::get()`.
- `deadline_exceeded` means the deadline expired while the call was still
  pending. The handle remains valid; the caller may issue another bounded wait
  or call `abandon()`.

Threading and lifecycle:
- The status reflects only the wait performed by the calling thread. Other
  callers waiting on the same handle observe the same eventual terminal state
  but may see different `Rpc_wait_status` values depending on their own
  deadlines.

Failures:
- None. `wait_until` does not throw based on the wait outcome; it only reports
  one of the two enumerators above.

Example source:
- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

See also:
- [Rpc_handle](rpc_handle.md)
- [Rpc_completion_state](rpc_completion_state.md)
