# Rpc_completion_state

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`Rpc_completion_state` is the lifecycle state of an `Rpc_handle<R>` returned by
`rpc_async_<method>(...)`. It captures whether the call is still in flight,
returned a value, observed a remote exception, was cancelled by the runtime,
or was abandoned by the caller.

Signature:
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

Use when:
- Inspecting an `Rpc_handle<R>` after a wait to decide how to consume the
  result.

Contract:
- `pending` means a final outcome has not yet been observed locally. The
  handle is still registered with the runtime.
- `returned` means a normal reply was received and `Rpc_handle<R>::get()` will
  yield the value.
- `remote_exception` means the remote handler threw a recognised exception.
  `Rpc_handle<R>::get()` rethrows the matching exception locally.
- `cancelled` means the wait ended because the runtime unblocked outstanding
  RPCs (typically during teardown or coordinator-loss handling).
  `Rpc_handle<R>::get()` throws `sintra::rpc_cancelled`.
- `abandoned` means the caller relinquished interest via
  `Rpc_handle<R>::abandon()` or by destroying a still-pending handle.
  `Rpc_handle<R>::get()` throws `std::runtime_error`.

Threading and lifecycle:
- The state is observed at the moment of inspection; once the handle leaves
  `pending` it does not transition further. Late-arriving remote results for
  abandoned or cancelled calls are discarded by the runtime.

Failures:
- None directly. The exceptions described above belong to
  `Rpc_handle<R>::get()`, not to inspecting the state.

Example source:
- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

See also:
- [Rpc_handle](rpc_handle.md)
- [Rpc_wait_status](rpc_wait_status.md)
- [rpc_cancelled](rpc_cancelled.md)
