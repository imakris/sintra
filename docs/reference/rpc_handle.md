# Rpc_handle

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`Rpc_handle<R>` is the result handle returned by `rpc_async_<method>(...)` for
an exported transceiver member function whose return type is `R`. It owns the
caller-side state of an in-flight transported RPC: the wait condition, the
final outcome (return value or exception), and the registration that lets the
local reader thread resolve the call.

Signature:
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

Use when:
- The caller wants to issue a remote method invocation without blocking.
- A bounded wait is required and the caller may want to abandon the call if the
  deadline expires.
- The caller wants to inspect the terminal state (returned, remote exception,
  cancelled, abandoned) before committing to retrieving a value.

Contract:
- `Rpc_handle<R>` is move-only. Default-constructed and moved-from handles
  hold no state; calling any operation on such a handle is well-defined and
  reflects "no pending RPC" semantics.
- `wait()` blocks until the call reaches a terminal state. It is safe to call
  more than once; subsequent calls return immediately.
- `wait_until(deadline)` returns `Rpc_wait_status::completed` when the call
  reached a terminal state at or before the deadline, otherwise
  `Rpc_wait_status::deadline_exceeded`. Repeated bounded waits are allowed;
  deadline expiry by itself does not change the terminal state.
- `ready()` returns `true` once the handle is no longer pending.
- `state()` reports the current `Rpc_completion_state` without consuming the
  value. While the call is pending it returns
  `Rpc_completion_state::pending`.
- `abandon()` causes the caller to give up interest in the result. It returns
  `true` only on the first transition from pending to abandoned and does not
  cancel work on the remote side; remote completion that arrives later is
  discarded. Destroying a still-pending handle is equivalent to calling
  `abandon()`.
- `get()` blocks if necessary, then either returns the reply value, rethrows
  the remote exception locally, throws `sintra::rpc_cancelled` for a cancelled
  call, or throws `std::runtime_error` for an abandoned or empty handle.

Threading and lifecycle:
- Methods are safe to call from the thread that owns the handle. Handles must
  not be invoked from inside a message handler dispatching the same RPC, since
  that would deadlock.
- A pending handle keeps an internal return-handler entry alive until it
  reaches a terminal state. `abandon()` and the destructor release that entry
  promptly.
- During Sintra teardown the runtime unblocks outstanding RPCs and transitions
  pending handles to `Rpc_completion_state::cancelled`.

Failures:
- `get()` rethrows the remote exception when the terminal state is
  `remote_exception`.
- `get()` throws `sintra::rpc_cancelled` when the terminal state is
  `cancelled`.
- `get()` throws `std::runtime_error` when the terminal state is `abandoned`,
  when the handle holds no state, or when called on a handle that is still
  pending and `wait()` was unable to acquire a final outcome.

Example source:
- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)
- [tests/finalize_async_rpc_lifecycle_test.cpp](../../tests/finalize_async_rpc_lifecycle_test.cpp)

See also:
- [SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST](rpc.md)
- [Rpc_wait_status](rpc_wait_status.md)
- [Rpc_completion_state](rpc_completion_state.md)
- [rpc_cancelled](rpc_cancelled.md)
