# sintra::local

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`local()` returns the maildrop bound to local recipients only (handlers in the
current process). Each `<<` operation wraps the value in a
`Message<Enclosure<T>>` and sends it as the managed-process transceiver, but
the resulting message is not delivered to other processes.

Signature:

```cpp
sintra::Maildrop<sintra::any_local>& sintra::local();
```

Use when:

- Routing a value to slots inside the current process without consuming the
  swarm-wide ring path.
- Building a same-process component bus while still using Sintra typed slots.
- The caller specifically wants to skip remote delivery for this message.

Contract:

- Only handlers registered in the current managed process are eligible to
  receive the message.
- The message type and sender contract is identical to
  [`sintra::world`](world.md); only the locality wildcard differs.
- Operator chaining is supported.

Threading and lifecycle:

- Requires a successfully initialized Sintra runtime. Call `sintra::init()`
  before sending through `local()`, and stop using the maildrop before runtime
  teardown.
- Slot handlers run on Sintra reader threads.

Failures:

- Compile-time error if the value type is not supported by the message
  serialiser. See [`message_payloads.md`](message_payloads.md).

Example source:

- [example/sintra/sintra_example_3_single_process.cpp](../../example/sintra/sintra_example_3_single_process.cpp)
  uses `sintra::world()`; replace with `sintra::local()` to restrict delivery
  to the same process.

See also:

- [sintra::world](world.md)
- [sintra::remote](remote.md)
- [sintra::Maildrop](maildrop.md)
- [sintra::activate_slot](activate_slot.md)
