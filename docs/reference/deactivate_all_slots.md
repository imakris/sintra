# sintra::deactivate_all_slots

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`deactivate_all_slots` releases every slot currently registered on the managed
process transceiver, including those registered through
[`sintra::activate_slot`](activate_slot.md) and through `Transceiver::activate`
on the managed process itself.

Signature:

```cpp
void sintra::deactivate_all_slots();
```

Use when:

- A process is finishing its participation and wants to drop callbacks
  before joining a final barrier or calling shutdown.
- A control thread wants to halt all slot-driven processing in one call,
  without tracking individual deactivators.

Contract:

- Affects only the managed-process transceiver of the calling process. User
  transceivers are unaffected; deactivate them through
  `Transceiver::deactivate_all` (see [`sintra::Transceiver`](transceiver.md)).
- Idempotent: calling it twice in succession leaves the second call as a
  no-op.

Threading and lifecycle:

- Call from a control thread, not from a slot handler.
- After `deactivate_all_slots()` returns, value-style slots registered via
  [`sintra::activate_slot`](activate_slot.md) are no longer dispatched.

Failures:

- None on the API surface. The function returns even if no slots were
  registered.

Example source:

- [example/sintra/sintra_example_1_ping_pong_multi.cpp](../../example/sintra/sintra_example_1_ping_pong_multi.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)

Reduced example (from `sintra_example_1_ping_pong_multi.cpp`):

```cpp
#include <sintra/sintra.h>

void wait_for_stop()
{
    sintra::barrier("stop slot activation barrier");
    sintra::receive<Stop>();
    sintra::deactivate_all_slots();
}
```

See also:

- [sintra::activate_slot](activate_slot.md)
- [sintra::receive](receive.md)
- [sintra::Transceiver](transceiver.md)
