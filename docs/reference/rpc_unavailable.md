# rpc_unavailable

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`sintra::rpc_unavailable` is the exception type thrown when an RPC cannot be
delivered to its target. It distinguishes target-side unavailability from
caller-side cancellation.

Signature:
```cpp
namespace sintra {

struct rpc_unavailable : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

} // namespace sintra
```

Use when:
- Catching the failure mode where the requested target is gone or unavailable.
- Distinguishing target teardown from `sintra::rpc_cancelled`, which represents
  runtime cancellation of the caller's outstanding wait.

Contract:
- `rpc_unavailable` derives from `std::runtime_error`, so a generic
  `catch (const std::runtime_error&)` clause matches it.
- The runtime uses this type when the target instance has been unpublished,
  destroyed, is shutting down, or its process is gone before the RPC can be
  delivered.
- The type identity is preserved across the RPC exception-serialization path, so
  remote unavailability can be caught as `sintra::rpc_unavailable` by the
  caller.

Threading and lifecycle:
- The exception is constructed on the calling thread when the RPC result is
  consumed. It is not thrown from a Sintra reader thread directly into user
  code.

Failures:
- N/A. The type itself is the failure signal.

Example source:
- [tests/rpc_unavailable_typed_test.cpp](../../tests/rpc_unavailable_typed_test.cpp)
- [tests/teardown_targeted_rpc_exception_test.cpp](../../tests/teardown_targeted_rpc_exception_test.cpp)

See also:
- [SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST](rpc.md)
- [rpc_cancelled](rpc_cancelled.md)
- [Rpc_handle](rpc_handle.md)
