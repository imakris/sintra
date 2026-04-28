# sintra::deactivate_all_slots

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
void sintra::deactivate_all_slots();
```

Description: Release every slot currently registered on the
managed-process transceiver, including those registered through
[`sintra::activate_slot`](activate_slot.md) and through
`Transceiver::activate` on the managed process itself.

## Returns

- `void`.

## Throws

- `std::runtime_error` when called without an active Sintra runtime.

## Use when

- A process is finishing its participation and wants to drop callbacks
  before joining a final barrier or calling
  [`sintra::shutdown`](shutdown.md).
- A control thread wants to halt all slot-driven processing in one
  call, without tracking individual deactivators.

## Contract

- Affects only the managed-process transceiver of the calling process.
  User transceivers are unaffected; deactivate them through
  `Transceiver::deactivate_all` (see [`sintra::Transceiver`](transceiver.md)).
- Idempotent. Calling it twice in succession leaves the second call as
  a no-op.
- Invalidates every outstanding deactivator returned by
  [`sintra::activate_slot`](activate_slot.md) for the slots it releases.
  Do not call those deactivators afterward.

## Threading and lifecycle

- Must be called from a control thread, not from a slot handler.
- After the function returns, value-style slots registered via
  [`sintra::activate_slot`](activate_slot.md) are no longer dispatched.

## Example

```cpp
#include <sintra/sintra.h>

void wait_for_stop()
{
    sintra::barrier("stop slot activation barrier");
    sintra::receive<Stop>();
    sintra::deactivate_all_slots();
}
```

## Example source

- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)

## See also

- [`sintra::activate_slot`](activate_slot.md)
- [`sintra::receive`](receive.md)
- [`sintra::Transceiver`](transceiver.md)
