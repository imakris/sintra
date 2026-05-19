# Message payloads

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

A Sintra message has two parts: a fixed `sintra::Message_prefix` carrying
addressing metadata and a body composed of user-supplied data. Every
maildrop value, transceiver message field, RPC argument, and RPC return value
must satisfy the message serialiser contract. This page is the central
payload checklist.

Signature (relevant types):

```cpp
namespace sintra {

using message_string = sintra::typed_variable_buffer<std::string>;

struct variable_buffer
{
    size_t num_bytes        = 0;
    size_t offset_in_bytes  = 0;

    bool empty() const;

    template <typename TC, typename T = typename TC::iterator::value_type>
    variable_buffer(const TC& container);
};

template <typename T>
struct typed_variable_buffer : protected variable_buffer
{
    typed_variable_buffer();
    typed_variable_buffer(const T& v);

    operator T() const;                  // materialises a copy of the contents
    size_t      size_bytes() const;
    size_t      data_offset_bytes() const;
    const void* data_address() const;
};

} // namespace sintra
```

Use when:

- Checking whether a type can cross a Sintra message or RPC boundary.
- Choosing the field type for `SINTRA_MESSAGE(name, fields...)`.
- Diagnosing template errors that mention unsupported message arguments.

## Allowed payload forms

| Form | Where it fits | Rule |
| --- | --- | --- |
| Plain arithmetic, enum, or aggregate value | Maildrop values, `SINTRA_MESSAGE` fields, RPC arguments, RPC returns | The type must be trivial and standard-layout. |
| Struct containing only allowed fixed-size fields | Maildrop values, `SINTRA_MESSAGE` fields, RPC arguments, RPC returns | The struct itself must remain trivial and standard-layout. |
| Type deriving from `sintra::Sintra_message_element` | Internal and generated message types | Use only when the public API already exposes such a type. |
| `std::string` | Top-level maildrop values and RPC arguments/returns | The serialiser stores it as variable-buffer data. |
| `std::vector<T>` | Top-level maildrop values and RPC arguments/returns | `T` must be trivially copyable. |
| `sintra::message_string` | `SINTRA_MESSAGE` fields or explicit variable-size message storage | String data stored inline with the message frame. |
| `sintra::typed_variable_buffer<T>` | `SINTRA_MESSAGE` fields or explicit variable-size message storage | `T::value_type` must be trivially copyable. |

For a quick self-check on a plain user struct:

```cpp
#include <type_traits>

struct Tick
{
    int    symbol;
    double price;
};

static_assert(std::is_trivial_v<Tick>);
static_assert(std::is_standard_layout_v<Tick>);
```

`SINTRA_MESSAGE` fields are literal fields in the generated message body. Use
`sintra::message_string` or `sintra::typed_variable_buffer<std::vector<T>>`
there instead of `std::string` or `std::vector<T>`.

Top-level maildrop sends and RPC calls are argument lists. Those paths can
transform variable-buffer-compatible arguments such as `std::string` and
`std::vector<int>` into the transported representation for you.

## Replacements for common non-payload shapes

| If you would write | Use instead | Reason |
| --- | --- | --- |
| `std::string` as a `SINTRA_MESSAGE` field | `sintra::message_string` | The message body stores the transported buffer descriptor, not a process-local string object. |
| `std::vector<T>` as a `SINTRA_MESSAGE` field | `sintra::typed_variable_buffer<std::vector<T>>` | The vector elements are stored inline with the message frame; `T` must be trivially copyable. |
| `std::vector<std::string>` | A flat byte encoding, separate messages, or a fixed schema using supported fields | Nested non-trivial containers are not valid variable-buffer payloads. |
| `std::map`, `std::unordered_map`, or other node containers | A flat vector of trivial records or separate messages | Container nodes and allocators are process-local. |
| `std::shared_ptr<T>` or `std::unique_ptr<T>` | Send the value, a stable id, or an application-level handle | Pointers and ownership control blocks do not cross a process boundary. |
| Raw pointer values | Send an id, offset, index, or copied value | A raw address is meaningful only inside the originating process. |
| `std::variant` of rich C++ objects | Separate message types, or an explicit tag plus supported fields | The active alternative and contained object layout are not a stable IPC schema. |
| Non-const reference RPC parameter | Return a value or send a response message | Mutable reference output parameters are rejected. |
| Reference RPC return type | Return by value | The referenced object would live in the callee process or stack frame. |

Contract:

- Fixed-size plain payload types must satisfy both
  `std::is_trivial_v<T>` and `std::is_standard_layout_v<T>`.
- Variable-buffer-compatible containers must expose `size()`, `begin()`, and
  `end()`, be constructible from an iterator range, and have a trivially
  copyable element type.
- `variable_buffer` only lives inside ring buffers. Application code generally
  interacts with `typed_variable_buffer<T>` or one of the declared aliases such
  as `message_string`.
- `typed_variable_buffer<T>::operator T()` copies the trailing bytes of
  the message into a freshly constructed `T`. It requires that the
  element type `T::value_type` is trivially copyable.
- The data referenced by a variable-buffer field is stored inline at the
  end of the message, immediately after the body. Lifetime is bounded by
  the surrounding message; once the handler returns or the
  [`sintra::receive`](receive.md) scope ends, the data may be reused by
  the ring writer.
- Copy variable-buffer fields out before any subsequent message
  processing if they must outlive the current handler or `receive`
  scope. Conversion to `T` (for example `std::string s = msg.text;`) is
  the standard way to do this.
- Element alignment is enforced by `typed_variable_buffer<T>` so that
  `data_address()` is suitable for direct access.
- Maildrop overloads accept C strings, string literals, fixed arrays,
  `std::string`, and `std::vector<T>` without manual wrapper construction.
  See [`sintra::Maildrop`](maildrop.md).
- RPC function declarations may use natural parameter types such as
  `const std::string&`; the transported request/reply message uses the
  serialised representation internally. RPC functions still must not return
  references or take non-const reference parameters.

Threading and lifecycle:

- Construction happens on the sending thread, in place inside the request
  ring. Reader threads observe the message after the writer publishes it.
- Variable-buffer fields share their lifetime with the enclosing message
  buffer; do not retain raw pointers from `data_address()` past the
  handler scope.

Failures:

- Compile-time error if a top-level maildrop value, RPC argument, or RPC return
  uses a type that does not satisfy the serialiser contract.

Common diagnostic:

```text
Unsupported message argument type. Provide a trivial standard-layout type, Sintra_message_element, or variable_buffer-compatible type.
```

- `SINTRA_MESSAGE` fields are stored exactly as written in the generated body.
  Unsupported field declarations may fail through regular C++ construction or
  generated-message constraints; use the explicit field replacements above
  instead of relying on automatic transformation.
- Compile-time error if an RPC declaration returns a reference or takes a
  non-const reference parameter.
- `std::runtime_error` or `std::overflow_error` when variable-buffer
  construction cannot represent the supplied payload size.
- Run-time assertion in debug builds when `typed_variable_buffer<T>` is
  read with a misaligned underlying buffer.

Example source:

- [tests/variable_buffer_alignment_test.cpp](../../tests/variable_buffer_alignment_test.cpp)
- [tests/message_args_traits_test.cpp](../../tests/message_args_traits_test.cpp)

Internal helpers (not for user code):

- `sintra::detail::message_args` is the storage type used internally for
  message argument packs. It may appear in template diagnostics but
  application code should not name it.

See also:

- [Transceiver messages](transceiver_messages.md)
- [Transceiver macros](transceiver_macros.md)
- [sintra::Maildrop](maildrop.md)
- [SINTRA_RPC / SINTRA_RPC_STRICT / SINTRA_UNICAST](rpc.md)
- [sintra::receive](receive.md)
