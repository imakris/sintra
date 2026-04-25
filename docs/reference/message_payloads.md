# Message payloads

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

A Sintra message has two parts: a fixed `sintra::Message_prefix` carrying
addressing metadata and a body composed of user-supplied fields. Body fields
are subject to a serialiser contract: they must be either trivial
standard-layout types, types deriving from `sintra::Sintra_message_element`,
or types convertible to `sintra::variable_buffer`. This page documents the
variable-buffer payload helpers available to application code.

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

- A message carries variable-length data such as a string, a vector of
  trivially-copyable elements, or a typed binary blob.
- The payload must travel inside a single message frame without external
  heap allocation.
- A typed wrapper is desired so that handlers can write a clear field type
  (`message_string`, `typed_variable_buffer<std::vector<int>>`).

Contract:

- `variable_buffer` only lives inside ring buffers. Application code
  generally interacts with `typed_variable_buffer<T>` or one of the
  declared aliases such as `message_string`.
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

Threading and lifecycle:

- Construction happens on the sending thread, in place inside the request
  ring. Reader threads observe the message after the writer publishes it.
- Variable-buffer fields share their lifetime with the enclosing message
  buffer; do not retain raw pointers from `data_address()` past the
  handler scope.

Failures:

- Compile-time error if a body field uses a type that does not satisfy
  the serialiser contract.
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
- [sintra::receive](receive.md)
