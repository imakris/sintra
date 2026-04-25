# sintra::Log_stream / set_log_callback

Include:

```cpp
#include <sintra/sintra.h>
```

Summary:

The Sintra logging surface is a small redirectable sink. `Log_stream`
collects formatted output through `operator<<` and dispatches the
accumulated text to the active log callback when destroyed.
`set_log_callback` installs a process-wide sink (or restores the default,
which writes to `stdout`/`stderr`). `log_raw` lets non-stream callers
push a precomposed message at a level. The four `ls_*` factories return
`Log_stream` instances at the matching level.

Signature:

```cpp
enum class log_level { error, warning, info, debug };

using log_callback_fn =
    void(*)(log_level level, const char* message, void* user_data);

void set_log_callback(log_callback_fn callback, void* user_data = nullptr);
log_callback_fn get_log_callback(void** user_data = nullptr);
void log_raw(log_level level, const char* message);

class Log_stream
{
public:
    explicit Log_stream(
        log_level level,
        bool enabled = true,
        std::string postfix = {});

    template <typename T>
    Log_stream& operator<<(const T& value);

    Log_stream(Log_stream&&) noexcept;
    Log_stream& operator=(Log_stream&&) noexcept;
};

Log_stream ls_info();
Log_stream ls_warning();
Log_stream ls_error();
Log_stream ls_debug();
```

Use when:

- Emitting structured warnings, errors, or debug traces from inside
  Sintra-aware code without taking a hard dependency on a specific
  output stream.
- Routing Sintra's own diagnostics into an existing application log
  (file, syslog, sink that fans out to multiple consumers) by
  installing a callback at startup.
- Composing a message piecemeal with `operator<<` while keeping the
  final flush atomic; the callback is invoked once when the
  `Log_stream` destructor runs.
- Adding a fixed suffix (for example, a trailing newline) by passing
  the `postfix` argument to the `Log_stream` constructor.

Contract:

- `set_log_callback(nullptr)` restores the built-in default callback,
  which writes errors and warnings to `stderr` and other levels to
  `stdout`, both flushed after each message.
- `set_log_callback` and `get_log_callback` serialise on an internal
  mutex; either may be called from any thread.
- `log_raw(level, message)` snapshots the current callback under the
  same mutex, then invokes the callback outside the lock. Exceptions
  thrown by the callback are swallowed.
- `Log_stream` owns its accumulation buffer. The destructor invokes the
  callback only when the stream is enabled and has non-empty content.
- `Log_stream` is move-constructible and move-assignable; the source
  becomes disabled and produces no output. It is not copyable.
- `enabled = false` produces no output and discards inserted values
  cheaply; useful for level-gated paths.
- The optional `postfix` is appended to the buffer just before
  invocation. After a move, the postfix transfers with the buffer; the
  source loses both its content and its postfix.

Threading and lifecycle:

- The callback may be invoked from any thread, including Sintra reader
  threads. Implementations must be thread-safe.
- `log_raw` is reentrant: the callback runs outside the registry lock.
- Replacing the callback while another thread is mid-call is safe in
  the sense that the old callback is invoked with the user-data it was
  registered with; the new callback takes over for subsequent calls.

Failures:

- Exceptions from the callback are caught by `log_raw` and discarded,
  so logging never propagates errors back into the caller.

Example source:

- [tests/log_stream_move_test.cpp](../../tests/log_stream_move_test.cpp)

See also:

- [sintra::console](console.md)
