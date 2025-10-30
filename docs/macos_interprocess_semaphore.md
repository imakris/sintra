# macOS `interprocess_semaphore` failure report

## Summary

The macOS semaphore backend crashed because our call to
`os_sync_wait_on_address_with_timeout` passed an invalid timeout argument. The trace that now
ships in `include/sintra/detail/interprocess_semaphore.h` shows that we converted relative
timeout durations into absolute Mach clock ticks before invoking the syscall. The Darwin API,
however, expects a *relative* timeout expressed in nanoseconds, regardless of the clock id we
select. By handing it a large absolute tick value (≈1.09×10¹² in the captured failure), the
kernel rejected the request with `EINVAL` and the test harness aborted.【F:include/sintra/detail/interprocess_semaphore.h†L55-L176】【F:include/sintra/detail/interprocess_semaphore.h†L629-L739】【F:docs/macos_interprocess_semaphore.md†L1-L65】

We now keep the timeout in nanoseconds and pass it straight to
`os_sync_wait_on_address_with_timeout`. This mirrors the contract documented in Apple's
`os_sync_wait_on_address.h`, eliminates the invalid argument error, and preserves the rest of
the semaphore logic. The trace remains enabled so future regressions can be diagnosed quickly.

## Failure timeline

The failing CI run produced the following key events:

1. A worker thread entered `wait_os_sync_with_timeout` with `expected = -1` (meaning it was
   waiting for the counter to become non-negative) and an 80 ms timeout.
2. The trace recorded
   `wait_os_sync_with_timeout attempt expected=-1 ... timeout_ns=79999584` and, in the legacy
   field that previously printed derived deadlines, reported
   `timeout_value=1093079754750`—the absolute Mach tick deadline we mistakenly forwarded to the
   kernel.
3. `os_sync_wait_on_address_with_timeout` returned `-1` and set `errno=22 (EINVAL)`.
4. The semaphore threw `std::system_error`, causing the macOS test binary to crash.

This sequence rules out mismatched wait/wake flags (the preceding untimed wait succeeded) and
points squarely at the malformed timeout argument.

## Fix

The fix removes the unused helpers that fabricated Mach absolute deadlines and updates the log
message to reflect that we now pass the plain nanosecond timeout straight to the syscall. No
behavioural changes are needed elsewhere because the timeout duration is already bounded and
validated inside `wait_os_sync_with_timeout`.【F:include/sintra/detail/interprocess_semaphore.h†L500-L516】【F:include/sintra/detail/interprocess_semaphore.h†L642-L739】

## Instrumentation usage

Tracing stays enabled by default. To redirect or disable it:

```bash
export SINTRA_OS_SYNC_TRACE_FILE="/tmp/sintra_os_sync_trace.log"  # optional; defaults to stderr
export SINTRA_OS_SYNC_TRACE=0   # optional; disables tracing entirely
```

Keep the trace active for the next macOS run to confirm that `errno=22` no longer appears.
