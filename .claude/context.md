# Project Context: Sintra

C++ header-only IPC library with multi-process testing infrastructure on Windows.

## Communication Guidelines
- Never use ✅ or similar celebratory characters
- Report objective facts - both what works and what doesn't
- If something doesn't work, state that first and focus on it
- Don't waste time re-proving trivial or already-working functionality just to deliver good news
- Don't conceal failures by only highlighting successes

## Project Structure
- `include/sintra/` - Header-only library
- `tests/` - Test infrastructure with Python harness
- `tests/manual/` - Intentionally failing/reproduction tests (built with BUILD_MANUAL_TESTS=ON)
- `tests/debuggers/` - Platform debugging: windows.py, unix.py, base.py

## Build Configurations
- `build-manual` - Manual tests with `-DBUILD_MANUAL_TESTS=ON`
- `build-test` - Regular tests
- `build-headeronly-test` - Header-only build validation

## Test Harness: tests/run_tests.py
- Uses CDB (Windows Debugger) to capture stack traces
- Supports multi-process tests via psutil for child PID enumeration
- Registry configuration for crash dumps and JIT debugging
- Live stack capture triggers on failure markers: `[FAIL]`, `assertion failed`, etc.

## Stack Capture Status (as of current session)
**What works:**
- Child process enumeration via psutil on Windows (tests/run_tests.py:1661-1677)
- Windows debugger callback integration (tests/debuggers/windows.py:664-669)
- Multi-process stack capture for processes still alive (verified with temp_ring_test: 1 parent + 2 workers)

**What doesn't work:**
- Stack capture from processes that have already exited (Windows limitation - no post-mortem without crash dumps)
- Tests that crash/exit quickly (coordinator_crash_children_hang_test, children_crash_coordinator_deadlock_test)
- barrier_delivery_fence_repro_test - all processes exit cleanly before stack capture triggers

**Workaround for failed tests:**
- Tests must either hang or print `[FAIL]` marker WHILE processes are still alive
- Otherwise stack capture only gets the parent after children have exited

## Manual Tests (tests/manual/)
- barrier_delivery_fence_repro_test.cpp - Delivery fence bug reproduction (exits cleanly, hard to capture stacks)
- hang_test.cpp - Single process infinite loop
- coordinator_crash_children_hang_test.cpp - Coordinator crashes, children hang
- children_crash_coordinator_deadlock_test.cpp - Children crash, coordinator deadlocks
- temp_ring.cpp - Test harness verification (hangs with [FAIL] marker)

## Debug Pause Feature (SINTRA_DEBUG_PAUSE_ON_EXIT)
**Status: Implemented and working**
- Environment variable: `SINTRA_DEBUG_PAUSE_ON_EXIT=1` enables opt-in crash pause
- When enabled, process pauses in infinite loop on crash/signal to allow debugger attachment
- Location: include/sintra/detail/runtime.h:89-254,280
- Windows: Uses vectored exception handler (AddVectoredExceptionHandler) + signal()
- Unix: Uses sigaction for SIGABRT, SIGSEGV, SIGFPE, SIGILL, SIGBUS, SIGTERM
- Handler prints PID and crash reason, then loops forever
- Verified working with access violation test

## Key Files Modified (current session)
- tests/run_tests.py:1661-1677 - Added psutil-based _collect_descendant_pids() for Windows
- tests/debuggers/windows.py:664-669 - Modified to use collect_descendant_pids callback
- tests/manual/barrier_delivery_fence_repro_test.cpp:236-238, 248-250, 282-283 - Added [FAIL] markers (still exits too fast)
- tests/manual/CMakeLists.txt - Added temp_ring.cpp test
- include/sintra/detail/runtime.h:89-254,280 - Added debug pause handlers with VEH on Windows
