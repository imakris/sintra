# macOS `interprocess_semaphore` failure analysis

## Symptom

Running the debug suite on macOS crashed in `sintra_interprocess_semaphore_test_debug` with an uncaught
`std::system_error` reporting `os_sync_wait_on_address_with_timeout: Invalid argument`. The failure was
reproducible in stress-style tests that exercise the `timed_wait` API, such as the multithreaded contention
scenario that waits with a 3 ms deadline on each iteration.【F:tests/interprocess_semaphore_test.cpp†L305-L347】

## Root cause

When the semaphore count dropped below zero, the macOS backend delegated timed waits to
`os_sync_wait_on_address_with_timeout`. The old implementation forwarded only the requested wait duration in
Mach ticks. However, the Darwin API expects an **absolute** deadline expressed in the time base of the
selected clock. Because the value we supplied was a tiny relative delta (for example ~3 million ticks for a
3 ms wait) instead of `mach_absolute_time() + delta`, the call was rejected with `EINVAL`. The runtime then
threw a `std::system_error` at the throw site in `wait_os_sync_with_timeout`, aborting the test process.【F:include/sintra/detail/interprocess_semaphore.h†L512-L551】

## Fix

We now convert the caller's deadline into an absolute timeout before invoking the kernel primitive. The
helper `current_wait_clock_ticks()` returns the current tick count for the clock used by the wait (Mach
absolute time on macOS), and `saturating_add()` guards against overflow when constructing the deadline. The
patched call site adds this current tick count to the converted duration, so the kernel receives a proper
absolute timestamp and the wait succeeds or times out as expected.【F:include/sintra/detail/interprocess_semaphore.h†L494-L551】

## Outcome

With the corrected deadline calculation the macOS timed-wait path no longer triggers `EINVAL`, allowing the
semaphore tests to complete successfully.
