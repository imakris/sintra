# macOS interprocess semaphore failure analysis

## Summary

The `interprocess_semaphore_test_debug` suite was crashing on macOS with an uncaught
`std::system_error` originating from `os_sync_wait_on_address_with_timeout`.
The kernel returned `EINVAL` because Sintra always asked the `os_sync` primitives
to treat the backing counter as a *shared* address, even when the semaphore lived in
process-private memory. Timed waits are stricter about this flag combination and
reject the request, so any call to `interprocess_semaphore::timed_wait` on a
non-shared semaphore aborted the test process.

The fix teaches the semaphore to fall back to "process local" wake/wait flags when
`EINVAL` is observed, while keeping the shared semantics for semaphores that really
are placed in shared mappings.

## Failure detail

* The exception originates from `interprocess_semaphore::wait_os_sync_with_timeout`
  at the `os_sync_wait_on_address_with_timeout` call (`interprocess_semaphore.h`, lines
  524-551). When the kernel rejects the wait request it sets `errno` to `EINVAL`.
* `std::system_error` is thrown on that line because the implementation only expected
  `EINTR`, `EFAULT`, or `ETIMEDOUT`, so any other error resulted in an exception.
* The macOS kernel considers it an error to use `OS_SYNC_WAIT_ON_ADDRESS_SHARED` for
  addresses that are not part of shared memory. Ordinary stack-allocated semaphores
  (used in the "basic semantics" test) fall into this category. Non-timed waits use
  `os_sync_wait_on_address`, which is more permissive, so the bug hid until the tests
  exercised the timed wait path.

## Fix

* `interprocess_semaphore` now tracks whether the `os_sync` counter is treated as
  shared or process-local (`interprocess_semaphore.h`, lines 397-424).
* When a wait, timed wait, or wake operation receives `EINVAL`, the implementation
  atomically switches the scope to process-local and retries (`interprocess_semaphore.h`,
  lines 526-625). This keeps shared semantics for mapped semaphores while allowing
  in-process semaphores to keep working.

## Additional notes

* The change is fully contained in the macOS-specific branch of the semaphore, so
  other platforms remain unaffected.
* If further issues appear, temporarily enabling `SINTRA_TRACE_OS_SYNC` and logging the
  observed scope transitions would highlight whether an unexpected code path is being
  taken.
