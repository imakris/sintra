# macOS `interprocess_semaphore` failure instrumentation

## Current status

The macOS implementation of `interprocess_semaphore` still fails the debug suite with
`os_sync_wait_on_address_with_timeout: Invalid argument`. We have not yet isolated the exact
root cause. Instead of speculating further, we added runtime instrumentation to capture the
state of every macOS wait/wake operation so the next test run can provide concrete evidence
about where the `EINVAL` originates.

## Instrumentation overview

The header `include/sintra/detail/interprocess_semaphore.h` now exposes a lightweight tracing
utility that records each interaction with the `os_sync_*` primitives when the
`SINTRA_OS_SYNC_TRACE` environment variable is set. The logger writes timestamped entries that
include:

- the semaphore counter value before and after each increment/decrement
- the arguments passed to `os_sync_wait_on_address[_with_timeout]`
- the computed timeout/deadline values used for timed waits
- the raw return codes and `errno` values reported by the operating system
- the count snapshot observed immediately after each syscall returns

This information should reveal which call site rejects our parameters and what values triggered
the kernel's `EINVAL` response.【F:include/sintra/detail/interprocess_semaphore.h†L55-L176】【F:include/sintra/detail/interprocess_semaphore.h†L468-L582】

## Enabling the trace

1. Build and run the failing tests with the environment variable set:
   ```bash
   export SINTRA_OS_SYNC_TRACE=1
   export SINTRA_OS_SYNC_TRACE_FILE="/tmp/sintra_os_sync_trace.log"  # optional; defaults to stderr
   ninja sintra_interprocess_semaphore_test_debug
   ./tests/sintra_interprocess_semaphore_test_debug
   ```
2. The trace is line-buffered. If `SINTRA_OS_SYNC_TRACE_FILE` is omitted, the log prints to
   standard error; otherwise, it appends to the supplied file path.
3. Share the resulting log so we can map the `EINVAL` to the exact wait attempt, expected
   counter value, and timeout supplied by the semaphore implementation.

## Next steps

Once we have a trace from a failing macOS run we can pinpoint the syscall that rejects our
inputs, correlate it with the surrounding semaphore logic, and implement a targeted fix with
confidence.
