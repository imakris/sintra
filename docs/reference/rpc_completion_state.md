# sintra::Rpc_completion_state

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
namespace sintra {

enum class Rpc_completion_state
{
    pending,
    returned,
    remote_exception,
    cancelled,
    abandoned,
};

} // namespace sintra
```

Description: Lifecycle state of an [`sintra::Rpc_handle<R>`](rpc_handle.md)
returned by `rpc_async_<method>(...)`. Captures whether the call is still
in flight, returned a value, observed a remote exception, was cancelled
by the runtime, or was abandoned by the caller.

## Enumerators

- `pending` — a final outcome has not yet been observed locally. The
  handle is still registered with the runtime.
- `returned` — a normal reply was received. `Rpc_handle<R>::get()` will
  yield the value.
- `remote_exception` — the remote handler threw a recognised exception.
  `Rpc_handle<R>::get()` rethrows the matching exception locally.
- `cancelled` — the wait ended because the runtime unblocked outstanding
  RPCs (typically during teardown or coordinator-loss handling).
  `Rpc_handle<R>::get()` throws [`sintra::rpc_cancelled`](rpc_cancelled.md).
- `abandoned` — the caller relinquished interest via
  `Rpc_handle<R>::abandon()` or by destroying a still-pending handle.
  `Rpc_handle<R>::get()` throws `std::runtime_error`.

## Throws

- Does not throw. Inspecting the state never throws; the exceptions
  described above belong to `Rpc_handle<R>::get()`.

## Use when

- Inspecting an `Rpc_handle<R>` after a wait to decide how to consume
  the result.

## Contract

- The state is observed at the moment of inspection. Once the handle
  leaves `pending` it does not transition further. Late-arriving remote
  results for abandoned or cancelled calls are discarded by the runtime.

## Example source

- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

## See also

- [`sintra::Rpc_handle`](rpc_handle.md)
- [`sintra::Rpc_wait_status`](rpc_wait_status.md)
- [`sintra::rpc_cancelled`](rpc_cancelled.md)
- [`sintra::rpc_unavailable`](rpc_unavailable.md)
- [`SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST`](rpc.md)
