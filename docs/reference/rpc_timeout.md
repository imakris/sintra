# sintra::rpc_timeout

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
namespace sintra {

struct rpc_timeout : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

} // namespace sintra
```

Description: Exception type thrown by
[`Rpc_handle<R>::get_until(deadline)`](rpc_handle.md) when the deadline
expires while the RPC is still pending and the handle successfully
abandons the caller-side result.

## Throws

- Does not throw on construction. The type itself is the failure signal;
  it is constructed and thrown by `Rpc_handle<R>::get_until(deadline)`.

## Use when

- Catching the bounded-result timeout path from `get_until(deadline)`.
- Distinguishing a local caller deadline from remote-side exceptions,
  runtime cancellation, or target unavailability.

## Contract

- Derives from `std::runtime_error`, so a generic
  `catch (const std::runtime_error&)` clause matches it.
- A timeout is local to the caller. It does not cancel remote execution.
  Late-arriving remote results are discarded after caller-side
  abandonment.
- If a terminal outcome wins after the deadline but before abandonment,
  `get_until(deadline)` preserves that outcome instead of throwing
  `rpc_timeout`.

## Threading and lifecycle

- The exception is constructed on the calling thread that consumes the
  bounded result. It is never thrown from a Sintra reader thread directly
  into user code.

## Example source

- [tests/rpc_bounded_result_test.cpp](../../tests/rpc_bounded_result_test.cpp)

## See also

- [`sintra::Rpc_handle`](rpc_handle.md)
- [`sintra::rpc_cancelled`](rpc_cancelled.md)
- [`sintra::rpc_unavailable`](rpc_unavailable.md)
- [`SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST`](rpc.md)
