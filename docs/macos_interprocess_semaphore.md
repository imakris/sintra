# macOS `interprocess_semaphore` failure instrumentation

## Current status

The trace from the most recent macOS failure shows that `os_sync_wait_on_address_with_timeout`
rejects the deadline that we submit immediately after a successful wake. The failing thread
records:

```
[... pid=16386 ...] wait_os_sync_with_timeout attempt expected=-1 expected_value=4294967295 remaining_ns=79999584 timeout_value=1093079754750 address=0x16b371d9c
[... pid=16386 ...] wait_os_sync_with_timeout result rc=-1 errno=22 count_snapshot=-1
```

`errno=22` (`EINVAL`) occurs even though the absolute deadline is well in the future. The wake
path uses the same address and flags successfully, so the only remaining degree of freedom is
the deadline encoding. This indicates that the version of macOS in CI interprets the final
parameter to `os_sync_wait_on_address_with_timeout` as a *relative interval* rather than an
absolute `mach_absolute_time` timestamp.

## Instrumentation overview

The header `include/sintra/detail/interprocess_semaphore.h` exposes a lightweight tracing
utility that records each interaction with the `os_sync_*` primitives. Tracing is enabled by
default so every macOS run captures the parameters and results of the failing calls. The logger
writes timestamped entries that include:

- the semaphore counter value before and after each increment/decrement
- the arguments passed to `os_sync_wait_on_address[_with_timeout]`
- the computed timeout/deadline values used for timed waits
- the raw return codes and `errno` values reported by the operating system
- the count snapshot observed immediately after each syscall returns

The new timeout strategy also logs when it switches between absolute deadlines and relative
intervals so we can verify which representation a given operating system accepts.【F:include/sintra/detail/interprocess_semaphore.h†L55-L132】【F:include/sintra/detail/interprocess_semaphore.h†L745-L825】

## Enabling the trace

1. Build and run the failing tests as usual. By default, macOS builds now emit the trace to
   standard error.
2. To redirect the log or disable it temporarily:
   ```bash
   export SINTRA_OS_SYNC_TRACE_FILE="/tmp/sintra_os_sync_trace.log"  # optional; defaults to stderr
   export SINTRA_OS_SYNC_TRACE=0   # optional; disables tracing entirely
   ```
3. Share the resulting log so we can map the `EINVAL` to the exact wait attempt, expected
   counter value, and timeout supplied by the semaphore implementation.

## Next steps

When the runtime observes `EINVAL` from an absolute deadline it automatically retries with a
relative interval and latches whichever mode succeeds. Subsequent waits reuse the verified
format, so once the CI run confirms which encoding macOS expects the failure should disappear.
If a new failure occurs the log will show both the rejected format and whether the dynamic
switch executed, narrowing any future investigations.
