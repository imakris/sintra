# sintra::console / sintra::Console

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

`sintra::Console` is a small RAII helper that buffers `operator<<`
output and, on destruction, sends the accumulated text to the
coordinator via `Coordinator::rpc_print`. The result is that `console()
<< ...` emits text on the coordinator's standard output regardless of
which managed process produced it. The free alias `sintra::console` is
the typical entry point and is intended to be used as a temporary
expression in a single statement.

Signature:

```cpp
namespace sintra {

class Console
{
public:
    Console();

    template <typename T>
    Console& operator<<(const T& value);
};

using console = Console;

} // namespace sintra
```

Use when:

- Emitting demonstration or progress text from worker processes that
  should appear on the coordinator's terminal in a sequenced way,
  rather than racing on each process's own `stdout`.
- Combining several values into one logical line, so the entire line
  is sent in a single coordinator RPC at the end of the statement.
- Writing examples or tests that need a single observable output
  channel without configuring the logging callback.

Contract:

- The class is non-copyable and non-movable. `operator new` overloads
  are deleted, so it cannot be heap-allocated; use it only as a
  temporary or local automatic variable.
- The accumulated text is sent when the `Console` destructor runs. A
  one-line statement like `sintra::console() << "msg";` issues exactly
  one print RPC at end-of-statement.
- The destructor catches all exceptions from `rpc_print`. Console
  output is best-effort: if the coordinator is unreachable (for
  example, during late teardown), the text is silently dropped.
- The text is delivered to the coordinator and printed there using its
  `rpc_print` implementation. From the worker's standpoint there is no
  guarantee about the absolute ordering of console messages from
  different processes beyond what the coordinator's own message
  ordering provides.
- `console()` is the spelling used in the examples; it is the type
  alias, not a free function call.

Threading and lifecycle:

- The destructor performs an RPC. Constructing a `Console` and not
  destroying it (for example, by binding it to a reference that
  outlives the local scope) defers the RPC; rely on temporaries to
  flush at end-of-statement.
- Calls before `init` or after teardown silently succeed because the
  coordinator RPC failure is swallowed.

Failures:

- None visible to the caller. RPC failures are caught and ignored
  inside the destructor.

Example source:

- [example/sintra/sintra_example_0_basic_pubsub.cpp](../../example/sintra/sintra_example_0_basic_pubsub.cpp)
- [example/sintra/sintra_example_4_recovery.cpp](../../example/sintra/sintra_example_4_recovery.cpp)
- [tests/console_test.cpp](../../tests/console_test.cpp)

See also:

- [sintra::Log_stream](logging.md)
