# sintra::remote

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`remote()` returns the maildrop bound to remote recipients only (handlers in
other managed processes). Each `<<` operation wraps the value in a
`Message<Enclosure<T>>` and sends it as the managed-process transceiver, but
the resulting message is not delivered to handlers in the current process.

Signature:

```cpp
sintra::Maildrop<sintra::any_remote>& sintra::remote();
```

Use when:

- Notifying peer processes about a state change without echoing the message
  to the current process.
- Implementing a producer-only pattern where local handling is unnecessary.

Contract:

- Only handlers registered in other managed processes are eligible to
  receive the message.
- The message type and sender contract is identical to
  [`sintra::world`](world.md); only the locality wildcard differs.
- Operator chaining is supported.

Threading and lifecycle:

- Requires a successfully initialized Sintra runtime. Call `sintra::init()`
  before sending through `remote()`, and stop using the maildrop before runtime
  teardown.
- Slot handlers run on Sintra reader threads in the receiving processes.

Failures:

- Compile-time error if the value type is not supported by the message
  serialiser. See [`message_payloads.md`](message_payloads.md).

Example source:

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
  uses `sintra::world()`; replace with `sintra::remote()` to suppress
  same-process delivery.

See also:

- [sintra::world](world.md)
- [sintra::local](local.md)
- [sintra::Maildrop](maildrop.md)
- [sintra::activate_slot](activate_slot.md)
- [sintra::Derived_transceiver](derived_transceiver.md)
