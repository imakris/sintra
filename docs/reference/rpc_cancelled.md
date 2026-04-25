# rpc_cancelled

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`sintra::rpc_cancelled` is the exception type thrown when an in-flight RPC is
unblocked by the Sintra runtime instead of completing normally. It indicates
that no remote reply will be observed for the call.

Signature:
```cpp
namespace sintra {

struct rpc_cancelled : std::runtime_error
{
    explicit rpc_cancelled(const char* what_arg);
};

} // namespace sintra
```

Use when:
- Catching the failure mode that corresponds to
  `Rpc_completion_state::cancelled`.
- Distinguishing runtime-driven cancellation from a remote-side exception or
  from a caller-driven abandonment.

Contract:
- `rpc_cancelled` derives from `std::runtime_error`, so a generic
  `catch (const std::runtime_error&)` clause matches it.
- The runtime may transition pending RPCs to cancelled during shutdown,
  coordinator loss, or other teardown paths that release outstanding waiters.
  Callers may also drive cancellation explicitly through internal
  unblock-rpc helpers exercised by lifecycle tests.
- `Rpc_handle<R>::get()` is the primary call site that throws this type.
  Synchronous `rpc_<method>(...)` may also surface it when the underlying wait
  is unblocked.

Threading and lifecycle:
- The exception is constructed on the calling thread when it consumes the
  cancelled state of a handle. It is never thrown from a Sintra-internal
  reader thread directly into user code.

Failures:
- N/A. The type itself is the failure signal.

Example source:
- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

See also:
- [Rpc_handle](rpc_handle.md)
- [Rpc_completion_state](rpc_completion_state.md)
