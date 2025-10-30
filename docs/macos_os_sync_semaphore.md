# macOS `os_sync` semaphore failure analysis

## Symptom
Running the macOS interprocess semaphore stress tests crashes with an uncaught
`std::system_error` when the implementation routes `timed_wait` through
`os_sync_wait_on_address_with_timeout` and the kernel reports `EINVAL` (invalid
argument). The exception is thrown directly inside the semaphore wait loop once
`os_sync_wait_on_address_with_timeout` fails, so the process aborts before the
fixture can report an assertion failure.【F:sintra/include/sintra/detail/interprocess_semaphore.h†L524-L561】

## Reproduction context
The failing scenario occurs in the cross-process coordination stress test.
Child processes repeatedly call `timed_wait` with a two second deadline while
arbitrating shared resources, ensuring that the wait implementation exercises
its timeout path on contended semaphores.【F:sintra/tests/interprocess_semaphore_test.cpp†L392-L451】

## Root cause
`os_sync_wait_on_address_with_timeout` interprets its timeout parameter as an
absolute deadline expressed in the clock domain supplied via `wait_clock`. The
previous implementation forwarded the relative nanosecond interval calculated by
`nanoseconds_to_mach_absolute_ticks` directly to the syscall. Because the value
represented a duration instead of a deadline, the kernel rejected the request
with `EINVAL`, which surfaced as the uncaught `std::system_error` seen in the
macOS test logs.【F:sintra/include/sintra/detail/interprocess_semaphore.h†L411-L428】【F:sintra/include/sintra/detail/interprocess_semaphore.h†L541-L560】

## Fix
The semaphore now converts relative timeouts into proper deadlines before
invoking `os_sync_wait_on_address_with_timeout`. On systems that expose
`OS_CLOCK_MACH_ABSOLUTE_TIME`, the code adds the converted tick delta to the
current `mach_absolute_time()` reading, saturating on overflow. On platforms
where only `OS_CLOCK_MONOTONIC`/`CLOCK_MONOTONIC` are available, it derives the
absolute deadline via `clock_gettime`. Both branches feed the resulting absolute
value back into the wait primitive, satisfying the API contract and eliminating
the invalid-argument failures observed on macOS.【F:sintra/include/sintra/detail/interprocess_semaphore.h†L431-L455】

## Validation
The Linux build still compiles and the interprocess semaphore regression test
suite passes after the change, covering the same cross-process coordination
scenario that previously failed on macOS.【37b5df†L1-L4】【37b5df†L5-L6】
