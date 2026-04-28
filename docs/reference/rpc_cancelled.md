# sintra::rpc_cancelled

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
namespace sintra {

struct rpc_cancelled : std::runtime_error
{
    explicit rpc_cancelled(const char* what_arg);
};

} // namespace sintra
```

Description: Exception type thrown when an in-flight RPC is unblocked by
the Sintra runtime instead of completing normally. It indicates that no
remote reply will be observed for the call.

## Throws

- Does not throw on construction. The type itself is the failure signal;
  it is constructed and thrown by Sintra when a pending RPC is cancelled.

## Use when

- Catching the failure mode that corresponds to
  `Rpc_completion_state::cancelled`.
- Distinguishing runtime-driven cancellation from a remote-side exception
  or from a caller-driven abandonment.

## Contract

- Derives from `std::runtime_error`, so a generic
  `catch (const std::runtime_error&)` clause matches it.
- The runtime may transition pending RPCs to cancelled during shutdown,
  coordinator loss, or other teardown paths that release outstanding
  waiters.
- `Rpc_handle<R>::get()` is the primary call site that throws this type.
  Synchronous `rpc_<method>(...)` may also surface it when the underlying
  wait is unblocked.

## Threading and lifecycle

- The exception is constructed on the calling thread when it consumes
  the cancelled state of a handle. It is never thrown from a Sintra
  reader thread directly into user code.

## Example source

- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

## See also

- [`sintra::Rpc_handle`](rpc_handle.md)
- [`sintra::Rpc_completion_state`](rpc_completion_state.md)
- [`sintra::rpc_unavailable`](rpc_unavailable.md)
- [`SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST`](rpc.md)
