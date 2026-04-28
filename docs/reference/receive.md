# sintra::receive

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
template <typename MESSAGE_T>
MESSAGE_T sintra::receive();

template <typename MESSAGE_T, typename SENDER_T>
MESSAGE_T sintra::receive(sintra::Typed_instance_id<SENDER_T> sender_id);
```

Description: Block the calling control thread until a message with the
requested body type arrives, then return the message value. The
sender-filtered overload accepts only messages emitted from a specific
transceiver instance.

## Parameters

- `MESSAGE_T` (template parameter) — the body type accepted by the slot.
  For values broadcast through [`sintra::world`](world.md),
  [`sintra::local`](local.md), or [`sintra::remote`](remote.md), this is
  the value type itself (for example `int` or `std::string`). For typed
  messages declared via `SINTRA_MESSAGE` it is the nested message type.
- `sender_id` (filtered overload) — the transceiver whose messages this
  receive call accepts. Other senders are ignored.

## Returns

- A value of type `MESSAGE_T`, holding the body of the first matching
  message.

## Throws

- Does not throw on a normal completion.
- Calling from a request-reader thread is a programmer error. In debug
  builds the function aborts with a diagnostic; in release builds the
  call deadlocks.

## Use when

- A top-level process function needs to wait synchronously for a single
  message before continuing.
- Bootstrapping handshakes such as exchanging instance ids before the
  main loop begins.
- Coordinating teardown by waiting for an explicit `Stop`-style signal.

## Contract

- The function activates a temporary slot internally, waits on a
  condition variable, then deactivates that slot before returning. Only
  one matching message is consumed per call.
- The sender-filtered overload only returns when the message is
  produced by the transceiver wrapped in `sender_id`.
- `receive` is not a handler API. Must not be called from inside another
  slot or RPC callback; the awaited message would have to be dispatched
  by the very thread that is blocked.

## Threading and lifecycle

- Must be called from a control thread (a process entry function or a
  thread that is not a Sintra reader thread).
- Variable-buffer-backed fields in the returned value reference ring
  memory. Copy them out before any subsequent message processing if
  they must outlive the current scope.
- The internal slot is removed automatically before the function
  returns. No deactivation by the caller is required.

## Notes

- Apply timeouts externally by signalling from another thread that
  emits the awaited message type. `receive` does not expose a deadline
  parameter.

## Example

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

## Example source

- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [tests/receive_test.cpp](../../tests/receive_test.cpp)

## See also

- [`sintra::activate_slot`](activate_slot.md)
- [`sintra::deactivate_all_slots`](deactivate_all_slots.md)
- [`sintra::world`](world.md)
- [`sintra::Typed_instance_id`](typed_instance_id.md)
- [Message payloads](message_payloads.md)
