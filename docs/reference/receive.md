# sintra::receive

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`receive<MESSAGE_T>()` blocks the calling control thread until a message with
the requested body type arrives, then returns the message value. The
sender-filtered overload accepts only messages emitted from a specific
transceiver instance.

Signature:

```cpp
template <typename MESSAGE_T>
MESSAGE_T sintra::receive();

template <typename MESSAGE_T, typename SENDER_T>
MESSAGE_T sintra::receive(sintra::Typed_instance_id<SENDER_T> sender_id);
```

Use when:

- A top-level process function needs to wait synchronously for a single
  message before continuing.
- Bootstrapping handshakes such as exchanging instance ids before the main
  loop begins.
- Coordinating teardown by waiting for an explicit `Stop`-style signal.

Contract:

- `MESSAGE_T` is the body type accepted by the slot. For values broadcast
  through [`sintra::world`](world.md), [`sintra::local`](local.md), or
  [`sintra::remote`](remote.md), this is the value type itself (for example
  `int` or `std::string`). For typed messages declared via `SINTRA_MESSAGE`
  it is the nested message type.
- The function activates a temporary slot internally, waits on a condition
  variable, then deactivates that slot before returning. Only one matching
  message is consumed per call.
- The sender-filtered overload only returns when the message is produced by
  the transceiver wrapped in `sender_id`.
- `receive` is not a handler API. Do not use it inside another slot or
  RPC callback; the awaited message would have to be dispatched by the very
  thread that is blocked.

Threading and lifecycle:

- Must be called from a control thread (a process entry function or a
  thread that is not a Sintra reader thread). In debug builds the function
  detects calls from request-reader threads and aborts with a diagnostic.
- Variable-buffer backed fields in the returned value reference ring memory.
  Copy them out before any subsequent message processing if they must
  outlive the current scope.
- The internal slot is removed automatically before the function returns.
  No deactivation by the caller is required.

Failures:

- Calling from a handler thread is a programmer error and will deadlock
  in release builds, or trip the debug-build guard.
- Returns only when a matching message arrives. Apply external timeouts
  by signalling from another thread that emits the awaited message type.

Example source:

- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [tests/receive_test.cpp](../../tests/receive_test.cpp)

Reduced example (from `tests/receive_test.cpp`, sender-filtered overload):

```cpp
#include <sintra/sintra.h>

int receiver_process(sintra::instance_id_type sender_a_id)
{
    auto msg = sintra::receive<DataMessage>(
        sintra::Typed_instance_id<sintra::Managed_process>(sender_a_id));
    // use msg.value, msg.score
    return 0;
}
```

See also:

- [sintra::activate_slot](activate_slot.md)
- [sintra::deactivate_all_slots](deactivate_all_slots.md)
- [sintra::world](world.md)
- [Message payloads](message_payloads.md)
