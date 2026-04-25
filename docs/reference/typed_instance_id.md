# Typed_instance_id

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`Typed_instance_id<T>` is a typed wrapper around a raw `instance_id_type` used
when activating slots and calling `sintra::receive`. The type parameter
constrains the kind of sender that the runtime accepts, and the
`Typed_instance_id<void>` specialisation carries non-typed group ids such as
`any_local`, `any_remote`, and `any_local_or_remote`.

Signature:
```cpp
namespace sintra {

template <typename T>
struct Typed_instance_id
{
    instance_id_type id;
    Typed_instance_id(const T& transceiver);
    Typed_instance_id(instance_id_type id_);
};

template <>
struct Typed_instance_id<void>
{
    instance_id_type id;
    Typed_instance_id(instance_id_type id_);
};

} // namespace sintra
```

Use when:
- Calling `activate_slot(handler, sender_id)` and you have a transceiver
  reference or a known instance id.
- Calling `receive<MESSAGE_T>(sender_id)` to wait for a message from a
  specific sender.
- Filtering on a wildcard group such as "all local senders" or "all remote
  senders".

Contract:
- `Typed_instance_id<T>` constructed from a `T&` reads the transceiver's
  `m_instance_id`, which makes it usable as a sender filter for any handler
  whose declared sender base is `T` or a base of `T`.
- The constructor that takes a raw `instance_id_type` stores the value
  directly, without type checking. Pass it the result of
  `instance_id()` for a typed sender, or one of the wildcard sentinels for
  the `void` specialisation.
- The default sender used by `activate_slot` when no explicit sender is given
  is `Typed_instance_id<void>(any_local_or_remote)`.
- `Typed_instance_id<T>::id` is a public field; consumers read it directly.

Threading and lifecycle:
- The wrapper holds only the id. Constructing it from a transceiver does not
  take ownership of, or reference-count, the transceiver. The caller is
  responsible for ensuring the referenced transceiver is alive at the moment
  of construction.

Failures:
- Construction does not validate the id; an `invalid_instance_id` value is
  accepted and produces a sender filter that matches no live sender.

Example source:
- [example/sintra/sintra_example_2_rpc_append.cpp](../../example/sintra/sintra_example_2_rpc_append.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)

See also:
- [Resolvable_instance_id](resolvable_instance_id.md)
- [Named_instance](named_instance.md)
- [instance ids](instance_ids.md)
