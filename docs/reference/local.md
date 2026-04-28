# sintra::local

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
sintra::Maildrop<sintra::any_local>& sintra::local();
```

Description: Return the maildrop bound to handlers in the current process
only. The shape of every operation matches [`sintra::world`](world.md);
the difference is purely locality — messages emitted through `local` are
not delivered to peer processes.

## Returns

- A reference to the process-wide `Maildrop<any_local>` static. See
  [`sintra::Maildrop`](maildrop.md) for the streaming API.

## Throws

- Compile-time error when the value type does not satisfy the message
  serialiser contract. See [Message payloads](message_payloads.md).

## Use when

- Routing a value to slots inside the current process without
  consuming the swarm-wide ring path.
- Building a same-process component bus while still using Sintra typed
  slots.
- The caller specifically wants to skip remote delivery for this
  message.

## Contract

- Only handlers registered in the current managed process are eligible
  to receive the message.
- The message type and sender contract are identical to
  [`sintra::world`](world.md); only the locality wildcard differs.

## Threading and lifecycle

- Same as [`sintra::world`](world.md). The accessor returns a reference
  to a function-local static, sending requires a successfully
  initialised Sintra runtime, and slot handlers run on Sintra reader
  threads.

## Example source

- [example/sintra/sintra_example_3_single_process.cpp](../../example/sintra/sintra_example_3_single_process.cpp)

## See also

- [`sintra::world`](world.md)
- [`sintra::remote`](remote.md)
- [`sintra::Maildrop`](maildrop.md)
- [`sintra::activate_slot`](activate_slot.md)
- [Message payloads](message_payloads.md)
