# sintra::Rpc_wait_status

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
namespace sintra {

enum class Rpc_wait_status
{
    completed,
    deadline_exceeded,
};

} // namespace sintra
```

Description: Result of a bounded wait on an [`sintra::Rpc_handle<R>`](rpc_handle.md).
Reports whether the wait observed a terminal state before the requested
deadline.

## Enumerators

- `completed` — the handle reached a terminal state at or before the
  deadline. Inspect it with `Rpc_handle<R>::state()` and retrieve the
  value (or rethrow the failure) with `Rpc_handle<R>::get()`.
- `deadline_exceeded` — the deadline expired while the call was still
  pending. The handle remains valid; the caller may issue another
  bounded wait or call `abandon()`.

## Throws

- Does not throw. `wait_until` does not throw based on the wait outcome;
  it only returns one of the two enumerators above.

## Use when

- Inspecting the result of `Rpc_handle<R>::wait_until(deadline)` to
  decide what to do next based on whether the call reached a terminal
  outcome.

## Contract

- The status reflects only the wait performed by the calling thread.
  Other callers waiting on the same handle observe the same eventual
  terminal state but may see different `Rpc_wait_status` values
  depending on their own deadlines.

## Example source

- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

## See also

- [`sintra::Rpc_handle`](rpc_handle.md)
- [`sintra::Rpc_completion_state`](rpc_completion_state.md)
- [`SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST`](rpc.md)
