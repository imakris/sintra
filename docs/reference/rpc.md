# SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
The RPC export macros are placed inside the body of a transceiver type that
derives from `sintra::Derived_transceiver<T>`. Each macro takes the name of an
existing member function and generates two static call wrappers as siblings of
the exported method:

- `rpc_<method>(target, args...)` is the blocking call form.
- `rpc_async_<method>(target, args...)` returns an `Rpc_handle<R>` for the
  asynchronous waiting form, where `R` is the return type of the exported
  method.

The macros differ only in whether a same-process call is allowed to take a
direct in-process shortcut and whether a reply is produced.

Signature:
```cpp
// All three macros are placed inside the transceiver class body.
SINTRA_RPC(method)         // direct shortcut allowed for same-process targets
SINTRA_RPC_STRICT(method)  // always uses the transported RPC path
SINTRA_UNICAST(method)     // void only, fire-and-forget, no reply produced

// The generated free functions, parameterised on the exported member function:
template <typename... Args>
static auto rpc_<method>(sintra::Resolvable_instance_id target, Args&&... args)
    -> R;

template <typename... Args>
static auto rpc_async_<method>(sintra::Resolvable_instance_id target, Args&&... args)
    -> sintra::Rpc_handle<R>;
```

Use when:
- A transceiver wants to expose member functions to peers in the same swarm.
- Local code wants a single call site that works whether the target lives in the
  current process or another process.
- The async-handle surface (deadline waits, abandonment, deferred result
  inspection) must work for a same-process target. In that case use
  `SINTRA_RPC_STRICT`, since `SINTRA_RPC` may pick a non-transported shortcut
  for which `rpc_async_<method>` is not supported.
- One-way notifications without a reply round-trip are needed. Use
  `SINTRA_UNICAST` for `void`-returning functions.

Contract:
- `target` is a `Resolvable_instance_id`. It accepts a raw `instance_id_type`
  (including `Typed_instance_id<T>::id` and the local form returned by
  `instance_id()`), a `std::string`, or a `const char*`. String forms resolve
  through the coordinator on first use.
- `SINTRA_RPC` may take a direct in-process call shortcut for blocking
  `rpc_<method>` invocations whose target resolves to the same process.
- `SINTRA_RPC_STRICT` always sends the request over the transport, even when
  the target is local. This is the export to use when local async behaviour
  must match the remote async behaviour.
- `SINTRA_UNICAST` exports a fire-and-forget message: the body must return
  `void`, no reply is generated, and `rpc_async_<method>` is rejected at
  compile time. Exceptions thrown by the handler are not visible to the caller.
- Same-process `rpc_async_<method>` is rejected at runtime when the export is
  `SINTRA_RPC` (the non-strict path); use `SINTRA_RPC_STRICT` for transported
  same-process async RPC, or call `rpc_<method>` for the blocking non-strict
  path.
- Member functions exported through any of these macros may not return a
  reference type and may not take non-`const` reference parameters; both are
  rejected at compile time.
- Calling `rpc_<method>` with `invalid_instance_id` throws `std::runtime_error`.
- A remote handler that throws a recognised `std::exception` subclass causes
  the matching exception to be rethrown locally inside `rpc_<method>` (or in
  `Rpc_handle<R>::get()` for the async form).

Threading and lifecycle:
- Call sites can be on any user thread that is not currently inside a Sintra
  message handler dispatch. The transceiver runtime must be initialised
  (`sintra::init` has succeeded) and the local managed process must still be
  live.
- The remote side runs the handler on a Sintra reader thread. RPC handlers
  observe the calling object's lifetime; an instance that is being destroyed
  will refuse new RPCs and reply with an unavailability exception to in-flight
  callers.
- A `SINTRA_UNICAST` send completes locally as soon as the message has been
  written to the request ring; there is no reply or cross-process
  synchronisation.

Failures:
- `std::runtime_error` when called before `sintra::init` or after teardown,
  when the target id is `invalid_instance_id`, or when same-process
  `rpc_async_<method>` is attempted against a `SINTRA_RPC` (non-strict)
  export.
- `sintra::rpc_unavailable` when the target instance has been unpublished,
  destroyed, is shutting down, or its process is gone. The type derives from
  `std::runtime_error`.
- `sintra::rpc_cancelled` when the wait is unblocked by coordinator loss or
  shutdown drain logic before a reply arrives.
- The remote-side exception (any of the recognised `std::` types) when the
  exported function throws.

Example source:
- [example/sintra/sintra_example_2_rpc_append.cpp](../../example/sintra/sintra_example_2_rpc_append.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)

See also:
- [Rpc_handle](rpc_handle.md)
- [Rpc_wait_status](rpc_wait_status.md)
- [Rpc_completion_state](rpc_completion_state.md)
- [rpc_cancelled](rpc_cancelled.md)
- [rpc_unavailable](rpc_unavailable.md)
- [Resolvable_instance_id](resolvable_instance_id.md)
