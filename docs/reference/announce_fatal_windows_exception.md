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
- Calling this API is idempotent with respect to Sintra's own fault paths. Those
  paths may already have dispatched by the time the host filter runs, and a
  duplicate or stale crash message cannot produce a second lifecycle event.
- POSIX signal behavior is unchanged. A hardware fault there is delivered as a
  signal on the faulting thread, so the process-wide handler already covers every
  thread and there is nothing for a host to announce.
- On Windows, Sintra consults the host's filter itself on both of its own fault
  paths, so this API remains necessary only for faults on threads Sintra does not
  own. See "Windows fault handling" below.

Windows fault handling:

Sintra has two fault paths of its own, and both give the host the final say:

- The CRT signal path, which the UCRT enters for a main-thread hardware fault.
- A per-thread structured-exception frame on the threads Sintra owns - the ring
  request and reply readers, recovery runners, and owned lifecycle workers -
  because Windows raises no signal for a fault on those threads. This frame
  requires `SINTRA_HAS_SEH`, which defaults to `1` for MSVC and clang-cl and to
  `0` elsewhere.

Both paths call `UnhandledExceptionFilter` before declaring anything, so:

- A fault the host's filter repairs (`EXCEPTION_CONTINUE_EXECUTION`) is not a
  death: nothing is dispatched, no readers are stopped, and execution resumes.
- A fault the host declines while a debugger is attached is left to the debugger,
  and is not reported.
- Only once the host has decided that execution will not continue does Sintra
  dispatch the abnormal termination and end the process, using the exception code
  as the exit status.

Two deliberate exceptions to "the host decides first":

- `EXCEPTION_STACK_OVERFLOW` is reported without consulting the host. What
  remains of the stack once the guard page is gone is enough for Sintra's bounded
  dispatch but not for an arbitrary crash reporter, and a fault inside the host's
  filter would lose the notification altogether.
- The host call is bounded by a watchdog, `SINTRA_CRASH_WATCHDOG_GRACE_MS`
  (default 5000). A host filter that blocks longer than that does not prevent the
  process from dying, so a faulted peer never lingers. Raise it if your crash
  reporter needs longer to write a minidump.

A faulted process is therefore guaranteed to be gone within the grace period of
the fault, but is not necessarily gone the instant its crash message is observed.

Example source:

- [tests/lifecycle_handler_test.cpp](../../tests/lifecycle_handler_test.cpp)
- [tests/recovery_test.cpp](../../tests/recovery_test.cpp)

See also:

- [sintra::set_lifecycle_handler](lifecycle_hooks.md)
- [sintra::recovery](recovery.md)
