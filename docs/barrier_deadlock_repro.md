# Reproducing the Barrier Delivery Fence Deadlock on Linux

This document captures a deterministic recipe for reproducing the Windows CI
hang that started after PR #605. The failure involves `wait_for_delivery_fence`
waiting on request reader progress after a barrier RPC has completed, creating
a distributed dependency cycle when every process reaches the barrier
simultaneously.

## Overview

The repository now provides a lightweight test hook that injects an artificial
sleep inside `Ring_R<T>::wait_for_new_data()` whenever the environment variable
`SINTRA_TEST_RING_DELAY_MS` is set to a positive integer. The hook is compiled
in for the test binaries and is otherwise inert. When a sufficiently large delay
is injected, the reader threads become slow enough for
`sintra_barrier_complex_choreography_test_debug` to deadlock on Linux in the
same way it does on Windows CI.

## Steps

1. Configure and build the debug test binaries:

   ```bash
   cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
   cmake --build build/debug --target sintra_barrier_complex_choreography_test_debug
   ```

2. Run the complex choreography test with an injected delay. Values above
   ~40 ms reliably trigger the hang; the example below uses 50 ms and terminates
   the run after 60 seconds so the command returns:

   ```bash
   SINTRA_TEST_RING_DELAY_MS=50 timeout 60s \
     build/debug/tests/sintra_barrier_complex_choreography_test_debug
   ```

   The command exits with code `124`, indicating that `timeout` killed the test
   after it became stuck. The test binary produces no additional diagnostics –
   it simply stops making progress, matching the behaviour observed on the
   Windows runners.

3. Clear the environment variable (or leave it unset) to return to the normal
   behaviour where the test completes quickly:

   ```bash
   unset SINTRA_TEST_RING_DELAY_MS
   build/debug/tests/sintra_barrier_complex_choreography_test_debug
   ```

## Notes

* The hook is only compiled into the test targets (via
  `SINTRA_ENABLE_TEST_HOOKS`). Production builds remain unaffected unless that
  macro is defined explicitly.
* Lower delay values (e.g. 10–20 ms) increase the run time of the test without
  always deadlocking, which mirrors the intermittent nature of the CI failure.
* The same environment variable can be used with other barrier-heavy tests if
  additional coverage is required.
