# sintra::remote

Defined in: `<sintra/sintra.h>`

Synopsis:

```cpp
sintra::Maildrop<sintra::any_remote>& sintra::remote();
```

Description: Return the maildrop bound to handlers in other managed
processes only. The shape of every operation matches [`sintra::world`](world.md);
the difference is purely locality — messages emitted through `remote`
are not echoed back to handlers in the current process.

## Returns

- A reference to the process-wide `Maildrop<any_remote>` static. See
  [`sintra::Maildrop`](maildrop.md) for the streaming API.

## Throws

- Compile-time error when the value type does not satisfy the message
  serialiser contract. See [Message payloads](message_payloads.md).

## Use when

- Notifying peer processes about a state change without echoing the
  message to the current process.
- Implementing a producer-only pattern where local handling is
  unnecessary.

## Contract

- Only handlers registered in other managed processes are eligible to
  receive the message.
- The message type and sender contract are identical to
  [`sintra::world`](world.md); only the locality wildcard differs.

## Threading and lifecycle

- Same as [`sintra::world`](world.md). Slot handlers run on Sintra
  reader threads in the receiving processes.

## Example source

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)

## See also

- [`sintra::world`](world.md)
- [`sintra::local`](local.md)
- [`sintra::Maildrop`](maildrop.md)
- [`sintra::activate_slot`](activate_slot.md)
- [`sintra::Derived_transceiver`](derived_transceiver.md)
- [Message payloads](message_payloads.md)
