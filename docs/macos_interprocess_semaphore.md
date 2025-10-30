# macOS `interprocess_semaphore` timed wait failure

## Summary

The macOS build crashed inside `interprocess_semaphore::wait_os_sync_with_timeout` when the
call to `os_sync_wait_on_address_with_timeout` raised a `std::system_error` with
`errno == EINVAL`. The exception originates from the throw statement in
`wait_os_sync_with_timeout` once the syscall reports failure.【F:include/sintra/detail/interprocess_semaphore.h†L709-L737】

The trace emitted by the failing run showed that we submitted a timeout value of
`1093079754750`, which is the current `mach_absolute_time()` tick count plus the requested
relative duration. However, the Darwin implementation of `os_sync_wait_on_address_with_timeout`
expects a *relative* timeout expressed in Mach absolute ticks. The kernel treats large absolute
deadlines as invalid input and returns `EINVAL`, which our code propagated as an uncaught
exception.

## Root cause

`interprocess_semaphore::wait_os_sync_with_timeout` converted the caller-supplied
`std::chrono::nanoseconds` to Mach ticks and then added the current tick count before passing the
result to `os_sync_wait_on_address_with_timeout`. Reviewing Apple's `libplatform` sources shows
that `os_sync_wait_on_address_with_timeout` forwards the timeout directly to `__ulock_wait2`
without enabling the `ULF_DEADLINE` flag, so the kernel interprets the value as a relative
interval. By submitting an absolute deadline we violated that contract and received `EINVAL` from
the kernel, which immediately threw out of the semaphore wait path.【F:include/sintra/detail/interprocess_semaphore.h†L688-L737】

## Fix

We now pass the relative duration (converted to Mach ticks) straight into
`os_sync_wait_on_address_with_timeout` instead of adding the current clock tick count. This keeps
the timeout within the range the kernel expects and allows timed waits to complete normally.
The instrumentation log still records every wait attempt so future regressions can be diagnosed
quickly.【F:include/sintra/detail/interprocess_semaphore.h†L688-L737】【F:include/sintra/detail/interprocess_semaphore.h†L55-L176】

## Instrumentation controls

Tracing remains enabled by default on macOS. Use the following environment variables to adjust
its behaviour when collecting evidence for future issues:

```bash
export SINTRA_OS_SYNC_TRACE_FILE="/tmp/sintra_os_sync_trace.log"  # optional; defaults to stderr
export SINTRA_OS_SYNC_TRACE=0   # optional; disables tracing entirely
```

With the corrected timeout computation the semaphore no longer throws, and the trace confirms
the kernel accepts the new parameters.
