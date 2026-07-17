# sintra::announce_fatal_windows_exception

Include:

```cpp
#include <sintra/sintra.h>
```

Signature (Windows only):

```cpp
void announce_fatal_windows_exception(std::uint32_t exception_code) noexcept;
```

`announce_fatal_windows_exception` lets a Windows host report a hardware
exception after the host has made the final decision that execution will not
continue. Sintra maps supported access-violation, illegal-instruction, and
arithmetic exception codes to its existing abnormal-termination dispatch.

Contract:

- Sintra does not install or own the process unhandled-exception filter.
- Call this API only from the host's terminal exception path. Do not call it
  before a saved filter or another recovery policy has had an opportunity to
  return `EXCEPTION_CONTINUE_EXECUTION`.
- Unknown exception codes and calls while the Sintra runtime is inactive are
  no-ops.
- Delivery is bounded and best-effort because it runs in a failing process.
- The CRT signal path and POSIX signal behavior are unchanged.

Example source:

- [tests/lifecycle_handler_test.cpp](../../tests/lifecycle_handler_test.cpp)
- [tests/recovery_test.cpp](../../tests/recovery_test.cpp)

See also:

- [sintra::set_lifecycle_handler](lifecycle_hooks.md)
- [sintra::recovery](recovery.md)
