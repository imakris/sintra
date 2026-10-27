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
the Sintra runtime or its caller abandons the wait through
`Rpc_handle<R>::get(std::stop_token)`. It indicates that no remote reply
will be observed for the call.

## Throws

- Does not throw on construction. The type itself is the failure signal;
  it is constructed and thrown by Sintra when a pending RPC is cancelled.

## Use when

- Distinguishing runtime cancellation or a caller-requested stop from a
  remote-side exception or caller-side timeout abandonment.

## Contract

- Derives from `std::runtime_error`, so a generic
  `catch (const std::runtime_error&)` clause matches it.
- The runtime may transition pending RPCs to cancelled during shutdown,
  coordinator loss, or other teardown paths that release outstanding
  waiters.
- `Rpc_handle<R>::get()` is the primary call site that throws this type.
  Synchronous `rpc_<method>(...)` may also surface it when the underlying
  wait is unblocked.
- `Rpc_handle<R>::get(stop)` also throws it when a requested stop wins
  caller-side abandonment. The remote work is not cancelled; if a terminal
  result or failure commits before abandonment, the overload preserves that
  outcome.

## Threading and lifecycle

- The exception is constructed on the calling thread when it consumes
  the cancelled state of a handle or abandons it after a stop request. It
  is never thrown from a Sintra reader thread or stop callback directly
  into user code.

## Example source

- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

## See also

- [`sintra::Rpc_handle`](rpc_handle.md)
- [`sintra::rpc_unavailable`](rpc_unavailable.md)
- [`SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST`](rpc.md)
