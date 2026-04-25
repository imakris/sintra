# Named_instance

Include:
```cpp
#include <sintra/sintra.h>
```

Summary:
`Named_instance<T>` is a `std::string`-derived wrapper that carries the
expected sender type alongside a published name. It is the parameter type of
the `activate(slot, named_instance)` overload that registers a slot which is
activated as soon as a transceiver of type `T` (or a derived type) is
published under that name.

Signature:
```cpp
namespace sintra {

template <typename T>
struct Named_instance : std::string
{
    Named_instance(const std::string& rhs);
    Named_instance(std::string&& rhs);
};

// Available on every Derived_transceiver<T>:
template <typename T, typename Parent = void>
struct Derived_transceiver : Parent
{
    static Named_instance<Transceiver_type> named_instance(const std::string& name);
    // ...
};

} // namespace sintra
```

Use when:
- Activating a slot against a sender that may not be published yet, but is
  expected to appear under a known name. Pass
  `Sender_type::named_instance("the name")` and Sintra will defer activation
  until the name is bound to a transceiver of that type (or one derived from
  it).
- A handler must explicitly state the sender type at the call site for
  readability or type-safety.

Contract:
- `Named_instance<T>` is constructible from any `std::string` value. The string
  is the published transceiver name to match.
- `Derived_transceiver<T>::named_instance(name)` is a static helper that
  returns `Named_instance<T>(name)`. It is the recommended construction site.
- The slot-activation overload that takes a `Named_instance<T>` does not
  require the sender to exist at registration time. The coordinator notifies
  the calling process once a matching publish event arrives, and the runtime
  performs the actual handler installation at that point.
- When the `sender_must_exist` template parameter of the activation overload
  is set to `true`, deferred activation is disabled and the call relies on the
  named transceiver being published already.

Threading and lifecycle:
- The deferred activation registration is owned by the calling transceiver and
  is removed when the transceiver is destroyed or when the activation
  deactivator is invoked.
- The matching publish event is emitted by the coordinator. The calling
  process therefore needs an active connection to the coordinator while the
  registration is outstanding.

Failures:
- Construction does not fail. If no transceiver of the expected type is ever
  published under the given name, the registration simply remains outstanding
  for the lifetime of the owner.

Example source:
- [example/qt_basic/cursor_sync_receiver.cpp](../../example/qt_basic/cursor_sync_receiver.cpp)

See also:
- [Resolvable_instance_id](resolvable_instance_id.md)
- [Typed_instance_id](typed_instance_id.md)
