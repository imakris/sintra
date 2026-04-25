# Resolvable_instance_id

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`Resolvable_instance_id` is the target parameter type used by every generated
`rpc_<method>(...)` and `rpc_async_<method>(...)` wrapper. It accepts either a
raw `instance_id_type` or a name and resolves the latter to an instance id by
asking the coordinator.

Signature:
```cpp
namespace sintra {

struct Resolvable_instance_id : Sintra_message_element
{
    Resolvable_instance_id(instance_id_type v);
    Resolvable_instance_id(const std::string& str);
    Resolvable_instance_id(const char* str);

    const Resolvable_instance_id& operator=(instance_id_type v);
    const Resolvable_instance_id& operator=(const std::string& str);

    operator instance_id_type() const;
};

} // namespace sintra
```

Use when:
- Calling `rpc_<method>(target, args...)` or
  `rpc_async_<method>(target, args...)` and supplying the target as either an
  id or a published name.

Contract:
- The raw `instance_id_type` constructor stores the value as-is. Pass the
  result of `Transceiver::instance_id()`, the conversion of a
  `Typed_instance_id<T>::id`, or any of the wildcard sentinels exposed in
  `id_types`.
- The `std::string` and `const char*` constructors call the coordinator
  (`Coordinator::resolve_instance`) with the supplied name. The result is the
  instance id of the published transceiver with that name, or
  `invalid_instance_id` if no such name is currently published.
- The conversion to `instance_id_type` yields the stored value. The default
  value is `invalid_instance_id`.
- Name resolution does not block waiting for the name to appear. If you need
  to block until a name is published, use `wait_for_instance` from the
  coordinator-side helpers; `Resolvable_instance_id` only performs lookup.

Threading and lifecycle:
- Constructing from a string contacts the coordinator. The runtime must be
  initialised, and the calling thread must not be inside a message handler
  whose dispatch the coordinator is also serving (which would deadlock).
- The conversion operator is a plain getter and is thread-safe with respect to
  itself.

Failures:
- Construction does not throw on resolution failure. An unknown name yields a
  stored `invalid_instance_id`; the receiving RPC call then fails with
  `std::runtime_error("Attempted to make an RPC call using an invalid instance ID.")`.
- Coordinator-side errors propagate as exceptions raised by the underlying
  RPC call.

Example source:
- [example/sintra/sintra_example_2_rpc_append.cpp](../../example/sintra/sintra_example_2_rpc_append.cpp)
- [example/sintra/sintra_example_6_unicast_send_to.cpp](../../example/sintra/sintra_example_6_unicast_send_to.cpp)
- [tests/rpc_async_lifecycle_test.cpp](../../tests/rpc_async_lifecycle_test.cpp)

See also:
- [SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST](rpc.md)
- [Typed_instance_id](typed_instance_id.md)
- [Named_instance](named_instance.md)
- [instance ids](instance_ids.md)
