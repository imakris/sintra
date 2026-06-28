# sintra::Rpc_handle

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
namespace sintra {

template <typename R>
class Rpc_handle
{
public:
    Rpc_handle();
    Rpc_handle(const Rpc_handle&) = delete;
    Rpc_handle(Rpc_handle&&) noexcept = default;
    Rpc_handle& operator=(const Rpc_handle&) = delete;
    Rpc_handle& operator=(Rpc_handle&& other) noexcept;
    ~Rpc_handle();

    template <typename Clock, typename Duration>
    auto get_until(
        const std::chrono::time_point<Clock, Duration>& deadline) -> R;

    auto get() const -> R;
};

} // namespace sintra
```

Description: Move-only result handle returned by `rpc_async_<method>(...)`
for an exported transceiver member function whose return type is `R`. It
owns the caller-side state of an in-flight transported RPC: the wait
condition, the final outcome (return value or exception), and the
registration that lets the local reader thread resolve the call.

## Members

- `get_until(deadline)` — wait until the deadline, then retrieve the value
  or rethrow the terminal failure. If the deadline expires and this handle
  wins the caller-side abandon race, throws [`sintra::rpc_timeout`](rpc_timeout.md).
  If a terminal outcome wins before abandonment, returns or throws
  exactly as `get()` would.
- `get()` — block if necessary, then return the reply value, rethrow the
  remote exception locally, or throw based on the terminal state (see
  *Throws* below).

## Throws

- The remote exception type, when the remote handler throws.
- [`sintra::rpc_cancelled`](rpc_cancelled.md) — when `get()` is called on
  a cancelled handle.
- [`sintra::rpc_timeout`](rpc_timeout.md) — when `get_until(deadline)`
  reaches its deadline and this handle successfully abandons the pending
  caller-side result.
- `std::runtime_error` — when `get()` is called on an abandoned handle
  or on a handle that holds no state.

## Use when

- A caller needs to issue a remote method invocation without blocking.
- A bounded result is required. Use `get_until(deadline)` for the normal
  "wait up to this deadline, then either get the result or time out" path.
- A caller wants to abandon local interest without cancelling remote work;
  destroy or replace the handle while it is still pending.

## Contract

- `Rpc_handle<R>` is move-only. Default-constructed and moved-from
  handles hold no state and may be destroyed or assigned a new state.
- `get_until(deadline)` combines a bounded wait, caller-side abandonment
  on deadline expiry, and result retrieval. If a terminal outcome wins
  the race against abandonment after the deadline, that outcome is
  preserved.
- Destroying a still-pending handle abandons caller-side interest.
- Late-arriving remote results for abandoned or cancelled calls are
  discarded by the runtime.

## Threading and lifecycle

- Methods are safe to call from the thread that owns the handle. Handles
  must not be invoked from inside a message handler dispatching the same
  RPC, since that would deadlock.
- A pending handle keeps an internal return-handler entry alive until it
  reaches a terminal state. `get_until(deadline)` timeout and the destructor
  release that entry promptly.
- During Sintra teardown the runtime unblocks outstanding RPCs; consuming
  such a handle throws [`sintra::rpc_cancelled`](rpc_cancelled.md).

## Example source

- [tests/rpc_bounded_result_test.cpp](../../tests/rpc_bounded_result_test.cpp)
- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

## See also

- [`SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST`](rpc.md)
- [`sintra::rpc_cancelled`](rpc_cancelled.md)
- [`sintra::rpc_timeout`](rpc_timeout.md)
- [`sintra::rpc_unavailable`](rpc_unavailable.md)
