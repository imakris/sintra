# macOS `interprocess_semaphore` failure instrumentation

## Root cause summary

The always-on trace paid off: the failing run captured the precise syscall arguments that
triggered macOS to abort the semaphore test. The relevant excerpt (timestamps truncated for
brevity) shows the timed wait supplying an enormous `timeout_value` (logged before the fix)
immediately before the kernel returned `EINVAL`:

```
wait_os_sync_with_timeout attempt expected=-1 expected_value=4294967295 remaining_ns=79999584 timeout_value=1093079754750 address=0x16b371d9c
wait_os_sync_with_timeout result rc=-1 errno=22 count_snapshot=-1
wait_os_sync_with_timeout throwing errno=22 expected=-1
```

According to Apple's public header, `os_sync_wait_on_address_with_timeout` expects the final
argument to be a *relative* timeout expressed in nanoseconds for the chosen clock id, not an
absolute deadline. We were passing `mach_absolute_time() + delta` because the API surface looks
similar to `os_sync_wait_on_address_with_deadline`. The kernel rightfully rejected those
out-of-range values with `EINVAL`.

## Fix

The macOS backend now forwards the remaining timeout directly, in nanoseconds, instead of converting it into an absolute `mach_absolute_time` tick count. When a kernel build still expects Mach tick units, the runtime detects the first `EINVAL`, flips a process-wide flag, and retries using converted tick values. If the kernel *still* refuses the call, the semaphore falls back to a cooperative polling loop that watches the counter, sleeps in short slices, and honours the original deadline. This keeps the documented fast path, restores compatibility with older kernels, and still provides bounded waits when both interpretations are rejected. a??F:include/sintra/detail/interprocess_semaphore.h�?�L500-L521a?`a??F:include/sintra/detail/interprocess_semaphore.h�?�L642-L790a?`

## Instrumentation overview

The trace hooks remain enabled so future macOS regressions will still produce detailed logs. The
logger records counter transitions, syscall parameters, and the OS responses for every wait and
wake path.【F:include/sintra/detail/interprocess_semaphore.h†L55-L176】【F:include/sintra/detail/interprocess_semaphore.h†L468-L582】

### Enabling or redirecting the trace

1. Build and run the macOS tests as usual; tracing emits to standard error by default.
2. Optional environment controls:
   ```bash
   export SINTRA_OS_SYNC_TRACE_FILE="/tmp/sintra_os_sync_trace.log"  # redirect output
   export SINTRA_OS_SYNC_TRACE=0                                    # disable tracing
   ```

### When to disable it

Disable tracing only when you need noiseless output (for example, benchmarking). Otherwise it is
lightweight and invaluable for diagnostics.



