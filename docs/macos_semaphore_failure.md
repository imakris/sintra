# macOS interprocess semaphore failure diagnosis

## Symptom

Running `sintra_interprocess_semaphore_test` on macOS crashed with an uncaught
`std::system_error`:

```
os_sync_wait_on_address_with_timeout: Invalid argument
```

The exception is raised from `sintra::detail::interprocess_semaphore::wait_os_sync_with_timeout`
when the implementation dispatches a timed wait to
`os_sync_wait_on_address_with_timeout` and the kernel returns `EINVAL`.

## Root cause

macOS expects the timeout that is passed to
`os_sync_wait_on_address_with_timeout` to be an **absolute** timestamp expressed
in the clock that is referenced by the `os_clockid_t` argument.  The previous
implementation forwarded a relative duration (the nanoseconds remaining until
the deadline).  Recent macOS releases reject such values with `EINVAL`, which
propagated out of the test suite as an uncaught exception.

The failure can be reproduced whenever any call to `interprocess_semaphore::timed_wait`
falls through to the macOS-specific slow path with a non-zero timeout.  That
includes the multi-threaded and cross-process sections of
`tests/interprocess_semaphore_test.cpp`.

## Fix

`include/sintra/detail/interprocess_semaphore.h` now converts every timeout to an
absolute timestamp before invoking `os_sync_wait_on_address_with_timeout`.
Depending on the available clock source (`OS_CLOCK_MACH_ABSOLUTE_TIME`,
`OS_CLOCK_MONOTONIC`, or `CLOCK_MONOTONIC`), we translate nanoseconds into the
appropriate tick units and add them to the current clock reading with
saturation.  This matches the contract required by the OS API and prevents the
`EINVAL` failures seen in CI.【F:include/sintra/detail/interprocess_semaphore.h†L408-L520】

## Additional instrumentation

Define `SINTRA_OS_SYNC_DIAGNOSTICS` during the build to emit diagnostic records
whenever Apple’s wait primitive still reports `EINVAL`.  The log captures the
expected counter value, the requested timeout, and the absolute tick values that
were passed to the system call.  This provides immediate visibility if future
platform revisions tighten the API further.【F:include/sintra/detail/interprocess_semaphore.h†L1-L12】【F:include/sintra/detail/interprocess_semaphore.h†L555-L567】

## Recommendations

* Keep the diagnostic flag handy while the fix soaks in CI; it is inexpensive
  and offers precise telemetry if the regression reappears.
* If additional failures occur, capture the emitted diagnostics to confirm that
  the clock/tick conversion still matches the kernel expectations.
