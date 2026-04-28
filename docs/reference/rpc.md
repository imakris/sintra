# SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
// All three macros are placed inside the body of a transceiver type that
// derives from sintra::Derived_transceiver<T>.
SINTRA_RPC(method)         // direct shortcut allowed for same-process targets
SINTRA_RPC_STRICT(method)  // always uses the transported RPC path
SINTRA_UNICAST(method)     // void only, fire-and-forget, no reply produced

// The RPC macros generate both wrappers. SINTRA_UNICAST generates the
// async wrapper too, but using it fails at compile time.
template <typename... Args>
static auto rpc_<method>(sintra::Resolvable_instance_id target, Args&&... args)
    -> R;

template <typename... Args>
static auto rpc_async_<method>(sintra::Resolvable_instance_id target, Args&&... args)
    -> sintra::Rpc_handle<R>;
```

Description: Export a transceiver member function as a remotely callable
RPC. Each macro takes the name of an existing member function and
generates `rpc_<method>` (blocking) and `rpc_async_<method>` (returning
[`sintra::Rpc_handle<R>`](rpc_handle.md)) as siblings of the exported
method. The macros differ only in whether a same-process call is allowed
to take a direct in-process shortcut and whether a reply is produced.

## Parameters

- `method` (macro argument) — the name of an existing member function on
  the enclosing transceiver. The export macro must be placed inside the
  class body; the named function must already be declared.
- `target` (generated wrappers) — a [`sintra::Resolvable_instance_id`](resolvable_instance_id.md).
  Accepts a raw `instance_id_type` (including `Typed_instance_id<T>::id`
  and the local id from `instance_id()`), a `std::string`, or a
  `const char*`. String forms resolve through the coordinator on first
  use.
- `args...` (generated wrappers) — forwarded to the exported member
  function on the target instance.

## Returns

- `rpc_<method>(...)` — the value returned by the remote handler, with
  type `R` (the exported function's return type). For `SINTRA_UNICAST`,
  `R` is `void` and the call returns immediately after writing the
  request to the ring.
- `rpc_async_<method>(...)` — a move-only [`sintra::Rpc_handle<R>`](rpc_handle.md)
  that resolves to the remote outcome. For `SINTRA_UNICAST`, the wrapper
  is generated but rejected at compile time with a `static_assert`.

## Throws

- `std::runtime_error` — when called before [`sintra::init`](init.md) or
  after teardown.
- `std::runtime_error` — when the target id is `invalid_instance_id` in
  `SINTRA_RPC_STRICT` or `rpc_async_<method>` calls. The fire-and-forget
  `SINTRA_UNICAST` blocking wrapper writes to the selected target id and
  does not perform this validation.
- `std::runtime_error` — when same-process `rpc_async_<method>` is
  attempted against a `SINTRA_RPC` (non-strict) export. Use
  `SINTRA_RPC_STRICT` for transported same-process async RPC.
- [`sintra::rpc_unavailable`](rpc_unavailable.md) — when the target
  instance has been unpublished, destroyed, is shutting down, or its
  process is gone.
- [`sintra::rpc_cancelled`](rpc_cancelled.md) — when the wait is
  unblocked by coordinator loss or shutdown drain logic before a reply
  arrives.
- The remote-side exception (any of the recognised `std::` types) when
  the exported function throws.

## Use when

- A transceiver needs to expose member functions to peers in the same
  swarm.
- Local code wants a single call site that works whether the target lives
  in the current process or another process.
- The async-handle surface (deadline waits, abandonment, deferred result
  inspection) must work for a same-process target. Use
  `SINTRA_RPC_STRICT`, since `SINTRA_RPC` may pick a non-transported
  shortcut for which `rpc_async_<method>` is not supported.
- One-way notifications without a reply round-trip are needed. Use
  `SINTRA_UNICAST` for `void`-returning functions.

## Contract

- The macro must be placed inside the body of a transceiver type that
  derives from `sintra::Derived_transceiver<T>`. The named member
  function must already exist.
- An exported member function must not return a reference type and must
  not take non-`const` reference parameters. Both are rejected at compile
  time.
- RPC argument and return types must satisfy the same payload contract as
  typed messages. Unsupported payloads fail to compile when the generated
  request or reply message is instantiated.
- `SINTRA_RPC` may take a direct in-process call shortcut for blocking
  `rpc_<method>` invocations whose target resolves to the same process.
- `SINTRA_RPC_STRICT` always sends the request over the transport, even
  when the target is local. Use this when local async behaviour must
  match the remote async behaviour.
- `SINTRA_UNICAST` requires a `void` return type. No reply is generated;
  calling its generated `rpc_async_<method>` wrapper is rejected at
  compile time. Exceptions thrown by the handler are not visible to the
  caller.
- A remote handler that throws a recognised `std::exception` subclass
  causes the matching exception to be rethrown locally inside
  `rpc_<method>` (or in `Rpc_handle<R>::get()` for the async form).

## Threading and lifecycle

- Call sites can run on any user thread that is not currently inside a
  Sintra message-handler dispatch. The transceiver runtime must be
  initialised ([`sintra::init`](init.md) has succeeded) and the local
  managed process must still be live.
- The remote side runs the handler on a Sintra reader thread. RPC
  handlers observe the calling object's lifetime; an instance that is
  being destroyed refuses new RPCs and replies with an unavailability
  exception to in-flight callers.
- A `SINTRA_UNICAST` send completes locally as soon as the message has
  been written to the request ring; there is no reply or cross-process
  synchronisation.

## Example source

- [example/sintra/sintra_example_2_rpc_append.cpp](../../example/sintra/sintra_example_2_rpc_append.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)

## See also

- [`sintra::Rpc_handle`](rpc_handle.md)
- [`sintra::Rpc_wait_status`](rpc_wait_status.md)
- [`sintra::Rpc_completion_state`](rpc_completion_state.md)
- [`sintra::rpc_cancelled`](rpc_cancelled.md)
- [`sintra::rpc_unavailable`](rpc_unavailable.md)
- [`sintra::Resolvable_instance_id`](resolvable_instance_id.md)
- [`sintra::Derived_transceiver`](derived_transceiver.md)
