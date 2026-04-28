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

    void wait() const;

    template <typename Clock, typename Duration>
    Rpc_wait_status wait_until(
        const std::chrono::time_point<Clock, Duration>& deadline) const;

    bool ready() const;
    Rpc_completion_state state() const;

    bool abandon();

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

- `wait()` — block until the call reaches a terminal state. Safe to call
  more than once; subsequent calls return immediately.
- `wait_until(deadline)` — return [`sintra::Rpc_wait_status`](rpc_wait_status.md)
  reflecting whether the call reached a terminal state at or before the
  deadline.
- `ready()` — return `true` once the handle is no longer pending.
- `state()` — return the current [`sintra::Rpc_completion_state`](rpc_completion_state.md)
  without consuming the value. Returns `pending` while the call is still
  in flight.
- `abandon()` — give up interest in the result. Returns `true` on the
  first transition from pending to abandoned. Does not cancel work on
  the remote side; remote completion that arrives later is discarded.
- `get()` — block if necessary, then return the reply value, rethrow the
  remote exception locally, or throw based on the terminal state (see
  *Throws* below).

## Throws

- The remote exception type, when the terminal state is `remote_exception`.
- [`sintra::rpc_cancelled`](rpc_cancelled.md) — when `get()` is called on
  a cancelled handle.
- `std::runtime_error` — when `get()` is called on an abandoned handle,
  on a handle that holds no state, or on a still-pending handle whose
  underlying wait could not produce a final outcome.
- `wait()` and `wait_until()` do not throw based on the wait outcome.

## Use when

- A caller needs to issue a remote method invocation without blocking.
- A bounded wait is required and the caller may want to abandon the call
  if the deadline expires.
- The caller wants to inspect the terminal state (returned, remote
  exception, cancelled, abandoned) before committing to retrieving a
  value.

## Contract

- `Rpc_handle<R>` is move-only. Default-constructed and moved-from
  handles hold no state; calling any operation on such a handle is
  well-defined and reflects "no pending RPC" semantics.
- Repeated bounded waits are allowed; deadline expiry by itself does not
  change the terminal state.
- Destroying a still-pending handle is equivalent to calling `abandon()`.
- Once the handle leaves `pending` it does not transition further;
  late-arriving remote results for abandoned or cancelled calls are
  discarded by the runtime.

## Threading and lifecycle

- Methods are safe to call from the thread that owns the handle. Handles
  must not be invoked from inside a message handler dispatching the same
  RPC, since that would deadlock.
- A pending handle keeps an internal return-handler entry alive until it
  reaches a terminal state. `abandon()` and the destructor release that
  entry promptly.
- During Sintra teardown the runtime unblocks outstanding RPCs and
  transitions pending handles to `Rpc_completion_state::cancelled`.

## Example source

- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

## See also

- [`SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST`](rpc.md)
- [`sintra::Rpc_wait_status`](rpc_wait_status.md)
- [`sintra::Rpc_completion_state`](rpc_completion_state.md)
- [`sintra::rpc_cancelled`](rpc_cancelled.md)
- [`sintra::rpc_unavailable`](rpc_unavailable.md)
