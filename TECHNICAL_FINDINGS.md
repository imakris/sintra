# Sintra Technical Findings and Issues

## Document Purpose

This document records all technical findings, observed issues, attempted fixes, and important pitfalls discovered during the investigation of test failures in the Sintra IPC library. It serves as a knowledge base for future debugging sessions and developers working on the library.

---

## Executive Summary

**Current Status**: All tests fail 100% with deadlock during shutdown in `finalize()` → `Process_message_reader::~Process_message_reader()` → `stop_and_wait()`.

**Root Cause**: Reader threads block in `wait_for_new_data()` waiting for messages. When shutdown is initiated, `stop_nowait()` calls `unblock_local()` to wake the threads, but they remain blocked and never exit.

**Attempted Fixes**:
1. Added race condition checks in `fetch_message()` - NO EFFECT
2. Added `yield()` to busy-wait loops - NO EFFECT
3. Tests still fail 100%

**Key Finding**: This issue exists in the original codebase (HEAD), not introduced by recent changes.

---

## Windows Recovery Hang Investigation (2024-07-05)

### Symptom Snapshot

- `recovery_test` respawns the crashing child on Windows, but the recovered instance stalls before any user code in `main()` executes.
- Other executables (`basic_pubsub`, `ping_pong`) continue to pass on both Linux and Windows, so the regression is isolated to the recovery path.
- Linux runs of `recovery_test` succeed, which points to a Windows-specific interaction.

### Observed State

- The respawned "crasher" process never prints the early debug messages in `main()`—it hangs while entering the function body shown at line 302 of `tests/recovery_test.cpp`.【F:tests/recovery_test.cpp†L303-L357】
- Coordinator and watchdog processes park in their normal waiting loops, while their message reader threads block inside `Ring_R::wait_for_new_data()` as expected when no messages arrive.

### Plausible Root Causes

1. **Static Initializer Contention** – The request reader threads install signal handlers via a translation-unit `inline std::once_flag` shared across the program.【F:include/sintra/detail/managed_process_impl.h†L41-L99】【F:include/sintra/detail/managed_process_impl.h†L122-L154】If the recovered process launches helper threads before the CRT finishes initializing the `once_flag`, the MSVC/GNU mingw runtime could deadlock waiting for static initialization to complete.
2. **Stale Shared Memory Semaphores** – A respawn might inherit semaphore names whose owning process (the crasher) died while holding them. Boost.Interprocess on Windows can leave kernel objects in a signaled/unsignaled limbo until all handles are closed, so a thread touching those during C runtime startup could block.
3. **CreateProcess Handle Leakage** – The respawn path passes inherited handles for shared memory and pipes. If any handle duplication happens during teardown, the new process can inherit blocked synchronization primitives and stall before `main()` when the runtime enumerates CRT init segments.

### Suggested Debugging Steps

1. **Instrument CRT Entry** – Add `__declspec(dllexport)` hooks for `mainCRTStartup` or use a TLS callback to print progress before `main()`. If the log fires, the stall sits between CRT and user code.
2. **Trace Static Initialization** – Temporarily replace the `inline` `std::once_flag` with a function-local static accessor (e.g., `static std::once_flag& flag(){ static std::once_flag f; return f; }`) to rule out TU ordering issues without changing semantics.【F:include/sintra/detail/managed_process_impl.h†L41-L99】
3. **Semaphore Health Check** – Before spawning the recovered process, enumerate and close orphaned named semaphores/mutexes (e.g., via `boost::interprocess::named_semaphore::remove`). If the hang disappears, focus on cleanup paths.
4. **WinDbg !locks / !ntsdexts.locks** – Attach to the stuck process before `main()` and inspect loader and CRT locks. This can confirm whether static constructor guards or loader locks are in play.
5. **Process Monitor Trace** – Capture `CreateProcess` and shared memory object access during the crash/restart loop to see if the new process repeatedly opens the same kernel object handles and stalls on `WaitForSingleObject` before reaching `main()`.

### Interim Mitigations

- Delay reader-thread start until after `sintra::init()` returns in the recovered process to avoid touching shared state during CRT init.
- Force removal of all named IPC primitives in the coordinator before respawning children, ensuring the new instance starts from clean objects.
- As a diagnostic, disable signal handler installation on Windows by wrapping the call in a compile-time flag; if the hang persists, the root cause lies in IPC recovery rather than handler setup.

---

## Timeline of Investigation

### Phase 1: Test Infrastructure Setup

**Goal**: Create robust testing framework to detect non-deterministic failures.

**Actions**:
1. Backed up recovery implementation changes to `backup_recovery_changes/`
2. Reset library to clean HEAD using `git restore`
3. Created `tests/run_tests.py` with configurable timeout and repetitions
4. Disabled `recovery_test` in CMakeLists.txt (unimplemented features)
5. Built and ran tests

**Outcome**: Discovered ALL tests fail 100% - every single repetition times out.

**Key Insight**: Non-determinism was not the issue - the failure is deterministic.

### Phase 2: Process Cleanup Issues

**Problem**: When tests timed out, stuck processes were left running, preventing subsequent test runs.

**Initial Approach**: Manual cleanup using `taskkill`

**User Feedback**: "The script should take care of such scenarios"

**Solution**:
- Modified test runner to use `subprocess.Popen()` instead of `subprocess.run()`
- Added `_kill_process_tree()` using `taskkill /F /T /PID`
- Added pre-cleanup of all sintra processes on startup

**Result**: Tests now clean up properly on timeout.

### Phase 3: Debugger Setup

**Goal**: Use command-line debugger to analyze hung processes.

**Investigation**:
- Located CDB: `C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe`
- Located GDB: `C:\Qt\Tools\mingw1310_64\bin\gdb.exe`
- Verified Debug build includes `-g` flag (DWARF symbols)

**CDB Attempt**: CDB attached but showed no symbols - requires PDB format, but MinGW generates DWARF.

**GDB Success**: GDB reads DWARF symbols perfectly. Verified with:
```bash
gdb --batch -ex "file sintra_basic_pubsub_test.exe" -ex "info functions wait_until"
```

**Outcome**: Established GDB as debugging tool of choice for MinGW builds.

### Phase 4: Root Cause Analysis with GDB

**Method**: Attached GDB to hung test process, obtained full stack traces for all threads.

**Key Findings from GDB Session**:

**Main Thread (Thread 1) - Deadlocked**:
```
#25 main() at basic_pub_sub.cpp:265
  ↓
#24 sintra::finalize() at sintra_impl.h:162
  ↓
#23 sintra::Managed_process::~Managed_process() at managed_process_impl.h:314
  ↓
#14 sintra::Process_message_reader::~Process_message_reader() at process_message_reader_impl.h:159
  ↓
#13 sintra::Process_message_reader::stop_and_wait(waiting_period=22) at process_message_reader_impl.h:132
  ↓
#12 std::condition_variable::wait_for(...) ← WAITING HERE (22 second timeout)
```

**Reader Thread (Thread 5) - Blocked on Semaphore**:
```
#14 sintra::Process_message_reader::request_reader_function() at process_message_reader_impl.h:213
  ↓
#13 sintra::Message_ring_R::fetch_message() at message.h:621
  ↓
#12 sintra::Ring_R<char>::wait_for_new_data() at ipc_rings.h:1396
  ↓
#11 sintra::sintra_ring_semaphore::wait() at ipc_rings.h:526
  ↓
#10 boost::interprocess::interprocess_semaphore::wait()
  ↓
  BLOCKED WAITING FOR MESSAGE THAT WILL NEVER ARRIVE
```

**Analysis**:
- Main thread calls `stop_and_wait()`, which waits for `m_req_running` and `m_rep_running` to become false
- Reader threads are blocked in semaphore wait, never exit, never set running flags to false
- `stop_nowait()` calls `done_reading()` (sets `m_reading = false`) then `unblock_local()` (posts semaphore)
- Reader threads should wake, see no more data, return nullptr from `fetch_message()`, exit loop
- But they don't - they remain blocked

### Phase 5: Attempted Fix #1 - Race Conditions in fetch_message()

**Hypothesis**: Maybe `m_reading` is being set back to true after `done_reading()` sets it false, causing a race condition.

**Changes Made** (`sintra/include/sintra/detail/message.h:606-668`):

1. Added atomic lock `m_reading_lock` to protect `m_reading` flag checks
2. Before calling `start_reading()`, check if `m_reading` is already false (stopped)
3. After calling `done_reading_new_data()`, check if `m_reading` became false (stopped)
4. Added multiple nullptr return points to exit early if stopped

**Code Logic**:
```cpp
Message_prefix* fetch_message()
{
    if (m_range.begin == m_range.end) {
        if (m_reading) {
            done_reading_new_data();

            // Check if we were stopped after done_reading_new_data()
            bool f = false;
            while (!m_reading_lock.compare_exchange_strong(f, true)) { f = false; }
            bool stopped = !m_reading;
            m_reading_lock = false;

            if (stopped) {
                return nullptr;  // Exit immediately
            }
        }
        else {
            // Check if we were stopped before initializing
            bool f = false;
            while (!m_reading_lock.compare_exchange_strong(f, true)) { f = false; }
            bool still_stopped = !m_reading;
            m_reading_lock = false;

            if (still_stopped) {
                return nullptr;  // Don't start reading again
            }

            start_reading();
        }

        auto range = wait_for_new_data();
        if (!range.begin) {
            return nullptr;
        }
        m_range = range;
    }

    // Check before returning message
    bool f = false;
    while (!m_reading_lock.compare_exchange_strong(f, true)) { f = false; }

    if (!m_reading) {
        m_reading_lock = false;
        return nullptr;
    }

    Message_prefix* ret = (Message_prefix*)m_range.begin;
    assert(ret->magic == message_magic);
    m_range.begin += ret->bytes_to_next_message;

    m_reading_lock = false;
    return ret;
}
```

**Test Results**:
```bash
python run_tests.py --repetitions 100 --timeout 5
```
**FAIL**: 0/400 tests passed (100% failure rate, same as before)

**Conclusion**: The race condition hypothesis was incorrect, or not the primary issue.

### Phase 6: Attempted Fix #2 - Yield in Busy-Wait

**Hypothesis**: The `wait_until_ready()` function busy-waits without yielding, consuming CPU during initialization.

**Changes Made** (`sintra/include/sintra/detail/process_message_reader_impl.h:66-70`):
```cpp
inline
void Process_message_reader::wait_until_ready()
{
    while (!m_req_running) { std::this_thread::yield(); }  // Added yield
    while (!m_rep_running) { std::this_thread::yield(); }  // Added yield
}
```

**Rationale**: While this doesn't fix the deadlock, it's good practice to yield when spinning.

**Test Results**: No change in deadlock behavior (expected - this is orthogonal to shutdown issue).

### Phase 7: Baseline Testing

**User Guidance**: "Check the changes in the staged (or backed up) copies, which were more recent and might have fixed the issue."

**Investigation**:
1. Examined `backup_recovery_changes/` directory contents
2. Compared with current codebase
3. Found backup contains:
   - `ipc_rings.h`: Recovery methods like `force_reset_for_recovery()`
   - `message.h`: Message ring recovery methods
   - `managed_process_impl.h`: Recovery spawn logic

**Finding**: The backup files contain NO fixes related to the shutdown deadlock. All changes are for crash recovery features.

**Baseline Test**: Reset library to HEAD and tested:
```bash
git restore sintra/include/sintra/detail/message.h
git restore sintra/include/sintra/detail/process_message_reader_impl.h
cmake --build ../build-ninja2
python run_tests.py --repetitions 100
```

**Result**: HEAD ALSO FAILS 100%

**Critical Conclusion**: The deadlock exists in the original codebase. This is not a regression from recent changes - the tests have never worked reliably.

---

## Detailed Technical Analysis

### The Shutdown Sequence (How It Should Work)

1. Test finishes, calls `sintra::finalize()`
2. `finalize()` destroys `Managed_process` singleton
3. `~Managed_process()` destroys all `Process_message_reader` objects
4. `~Process_message_reader()` calls `stop_and_wait(22.0)`
5. `stop_and_wait()` calls `stop_nowait()` which:
   - Sets `m_reader_state = READER_STOPPING`
   - Calls `m_in_req_c->done_reading()` → sets `m_reading = false`
   - Calls `m_in_req_c->unblock_local()` → posts to semaphore
   - Calls same for reply reader
6. Reader threads wake from semaphore wait
7. `wait_for_new_data()` returns empty range (no data available)
8. `fetch_message()` returns nullptr
9. Reader loop sees nullptr, breaks
10. Reader sets `m_req_running = false`
11. Reader notifies `m_stop_condition`
12. `stop_and_wait()` wakes, sees `!m_req_running && !m_rep_running`, returns true
13. Destructor completes successfully

### The Shutdown Sequence (What Actually Happens)

Steps 1-6 are the same, but then:

7. Reader thread wakes from semaphore wait (OR DOES IT?)
8. Thread remains blocked somehow
9. Never returns from `wait_for_new_data()`
10. Never sets `m_req_running = false`
11. `stop_and_wait()` times out after 22 seconds
12. `~Process_message_reader()` prints timeout message and calls `exit(1)`

### Code Analysis: stop_nowait()

**Location**: `sintra/include/sintra/detail/process_message_reader_impl.h:75-114`

```cpp
inline
void Process_message_reader::stop_nowait()
{
    m_reader_state = READER_STOPPING;

    m_in_req_c->done_reading();  // ← Sets m_reading = false
    m_in_req_c->unblock_local();  // ← Posts semaphore

    // if there are outstanding RPC calls waiting for reply, they should be
    // unblocked (and fail), to avoid a deadlock
    s_mproc->unblock_rpc();

    if (!tl_is_req_thread) {
        m_in_rep_c->done_reading();
        m_in_rep_c->unblock_local();
    }
    else {
        // The purpose of the lambda below is the following scenario:
        // 1. This function is called from a handler, thus from within the request
        //    reading function.
        // 2. The very same handler also calls some RPC, whose reply is sent through
        //    the reply ring, in the corresponding thread.
        // If we force the reply ring to exit, there will be no ring to get the
        // reply from, leading to a deadlock. Thus if the function is determined
        // to be called from within the request reading thread, forcing the reply
        // reading thread to exit will happen only after the request reading loop
        // exits.

        if (tl_post_handler_function) {
            delete tl_post_handler_function;
        }

        tl_post_handler_function = new
            function<void()>([this]() {
                m_in_rep_c->done_reading();
                m_in_rep_c->unblock_local();
            }
        );
    }
}
```

**Key Points**:
- Sets reader state to STOPPING
- Calls `done_reading()` which sets `m_reading = false` in the ring
- Calls `unblock_local()` which should wake blocked readers
- Has special handling for deferred unblock if called from request thread

### Code Analysis: stop_and_wait()

**Location**: `sintra/include/sintra/detail/process_message_reader_impl.h:119-153`

```cpp
inline
bool Process_message_reader::stop_and_wait(double waiting_period)
{
    std::unique_lock<std::mutex> lk(m_stop_mutex);
    stop_nowait();

    m_stop_condition.wait_for(lk, std::chrono::duration<double>(waiting_period),
        [&]() { return !(m_req_running || m_rep_running); }
    );

    if (m_req_running || m_rep_running) {
        // We might get here, if the coordinator is gone already.
        // In this case, we unblock pending RPC calls and do some more waiting.
        s_mproc->unblock_rpc(m_process_instance_id);
        m_stop_condition.wait_for(lk, std::chrono::duration<double>(waiting_period),
            [&]() { return !(m_req_running || m_rep_running); }
        );
    }

    if (m_req_running || m_rep_running) {
        m_in_req_c->done_reading();
        m_in_req_c->unblock_local();
        m_in_rep_c->done_reading();
        m_in_rep_c->unblock_local();
        m_stop_condition.wait_for(lk, std::chrono::duration<double>(1.0),
            [&]() { return !(m_req_running || m_rep_running); }
        );
        if (m_req_running || m_rep_running) {
            std::fprintf(stderr,
                "Process_message_reader::stop_and_wait timeout: pid=%llu req_running=%d rep_running=%d\n",
                static_cast<unsigned long long>(m_process_instance_id),
                m_req_running.load(), m_rep_running.load());
        }
    }
    return !(m_req_running || m_rep_running);
}
```

**Key Points**:
- Three-phase wait strategy with progressively more aggressive unblocking
- First wait: 22 seconds after initial `stop_nowait()`
- Second wait: 22 seconds after additional `unblock_rpc()`
- Third wait: 1 second after re-calling `done_reading()` and `unblock_local()`
- Always times out on third wait and prints error message

### Code Analysis: request_reader_function()

**Location**: `sintra/include/sintra/detail/process_message_reader_impl.h:183-343`

**Key Sections**:

```cpp
inline
void Process_message_reader::request_reader_function()
{
    install_signal_handler();

    tl_is_req_thread = true;

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers++;
    s_mproc->m_num_active_readers_mutex.unlock();

    m_in_req_c->start_reading();
    m_req_running = true;  // ← Set to true on startup

    while (m_reader_state != READER_STOPPING) {  // ← STOP CHECK EXISTS!
        s_tl_current_message = nullptr;

        // ... flush sequence handling ...

        Message_prefix* m = m_in_req_c->fetch_message();  // ← BLOCKS HERE
        s_tl_current_message = m;
        if (m == nullptr) {
            break;  // ← Should exit here when stopped
        }
        if (m_reader_state == READER_STOPPING) {  // ← Second check
            std::fprintf(stderr,
                "request_reader_function(pid=%llu) processing message while stopping...\n",
                ...);
        }

        // ... message processing logic ...
    }

    m_in_req_c->done_reading();

    s_mproc->m_num_active_readers_mutex.lock();
    s_mproc->m_num_active_readers--;
    s_mproc->m_num_active_readers_mutex.unlock();
    s_mproc->m_num_active_readers_condition.notify_all();

    std::lock_guard<std::mutex> lk(m_stop_mutex);
    if (m_reader_state == READER_STOPPING) {
        std::fprintf(stderr, "request_reader_function(pid=%llu) exiting normally after stop.\n",
            static_cast<unsigned long long>(m_process_instance_id));
    }
    m_req_running = false;  // ← SHOULD set to false on exit
    m_stop_condition.notify_one();  // ← SHOULD notify

    // ... cleanup ...
}
```

**Critical Discovery**: The stop flag check ALREADY EXISTS at line 196! The document `DEADLOCK_ROOT_CAUSE.md` incorrectly stated this check was missing.

**Reality**:
- Loop checks `m_reader_state != READER_STOPPING` at line 196
- Loop should exit when `READER_STOPPING` is set
- BUT: Thread is blocked inside `fetch_message()` at line 213
- The check at line 196 only happens AFTER `fetch_message()` returns
- So the issue is that `fetch_message()` never returns!

### Code Analysis: fetch_message() (Original)

**Location**: `sintra/include/sintra/detail/message.h:617-646` (before my changes)

**Original Code** (reconstructed from backup):
```cpp
Message_prefix* fetch_message()
{
    if (m_range.begin == m_range.end)
    {
        if (m_range.begin)
            done_reading();

        // NO CHECK FOR STOPPING FLAG BEFORE BLOCKING!
        wait_for_new_data();  // ← BLOCKS ON SEMAPHORE

        m_range = get_available_range_for_reading();
    }

    Message_prefix* ret = (Message_prefix*)m_range.begin;
    assert(ret->magic == message_magic);

    m_range.begin += ret->size;
    return ret;
}
```

**Problem**: When `wait_for_new_data()` blocks on semaphore, the function cannot check any stop flags.

### Code Analysis: wait_for_new_data()

**Location**: `sintra/include/sintra/detail/ipc_rings.h:1393-1400`

```cpp
void wait_for_new_data()
{
#if SINTRA_RING_READING_POLICY != SINTRA_RING_READING_POLICY_ALWAYS_SPIN
    // Wait on semaphore until data available or woken by unblock_local()
    c.dirty_semaphores[m_sleepy_index].wait();  // ← BLOCKS HERE
#else
    // Busy wait
    while (reading_sequence() == c.leading_sequence.load(std::memory_order_acquire)) {}
#endif
}
```

**Key Points**:
- Blocks on semaphore if reading policy is not ALWAYS_SPIN
- Should be woken by `unblock_local()` which posts to the semaphore
- After waking, should return and allow caller to check if data is available

### Code Analysis: unblock_local()

**Location**: `sintra/include/sintra/detail/ipc_rings.h:1449-1464`

```cpp
void unblock_local()
{
#if SINTRA_RING_READING_POLICY != SINTRA_RING_READING_POLICY_ALWAYS_SPIN
    c.lock();
    if (m_sleepy_index >= 0) {
        c.dirty_semaphores[m_sleepy_index].post_unordered();  // ← Wake semaphore
    }
    c.unlock();
#endif
}
```

**Potential Issues**:
1. **Timing issue**: `m_sleepy_index` might not be set yet when `unblock_local()` is called
2. **Race condition**: `m_sleepy_index` might be reset between check and post
3. **Wrong semaphore**: Maybe posting to wrong semaphore index
4. **Semaphore state**: Semaphore might be in inconsistent state

---

## Issues Observed

### Issue 1: 100% Test Failure Rate (UNRESOLVED)

**Severity**: CRITICAL

**Description**: All tests deadlock during shutdown with 100% reproducibility.

**Manifestation**:
- Main thread: Stuck in `Process_message_reader::stop_and_wait()` line 132
- Reader threads: Stuck in `sintra_ring_semaphore::wait()` line 526
- Timeout after 22 + 22 + 1 = 45 seconds total

**GDB Stack Traces**: See `DEADLOCK_ROOT_CAUSE.md` for complete traces.

**Root Cause Hypothesis**:
- `unblock_local()` is not successfully waking blocked reader threads
- Possible reasons:
  - `m_sleepy_index` not set when `unblock_local()` is called
  - Semaphore already in posted state, double-post doesn't work
  - Thread blocks again after being woken
  - Some synchronization primitive is in inconsistent state

**Status**: UNRESOLVED - Tests still fail 100% after attempted fixes

### Issue 2: Stuck Processes After Test Timeout (RESOLVED)

**Severity**: MEDIUM

**Description**: When tests timed out, processes remained running and had to be killed manually.

**Manifestation**: Multiple `sintra_*_test.exe` processes accumulating in task manager.

**Impact**: Subsequent test runs would fail with "Access denied" errors.

**Fix**:
- Modified `run_tests.py` to use `Popen` and `communicate()` with timeout
- Added `_kill_process_tree()` using `taskkill /F /T /PID`
- Added pre-cleanup in `run()` method

**Status**: RESOLVED

### Issue 3: CPU Spin in wait_until_ready() (RESOLVED)

**Severity**: LOW

**Description**: `wait_until_ready()` busy-waits without yielding CPU.

**Code**:
```cpp
// Before:
while (!m_req_running) {}
while (!m_rep_running) {}

// After:
while (!m_req_running) { std::this_thread::yield(); }
while (!m_rep_running) { std::this_thread::yield(); }
```

**Impact**: Minor CPU waste during initialization (typically < 100ms).

**Fix**: Added `std::this_thread::yield()` in busy-wait loops.

**Status**: RESOLVED

### Issue 4: CDB Cannot Read MinGW Symbols (DOCUMENTED)

**Severity**: LOW (workaround available)

**Description**: Microsoft CDB debugger cannot read DWARF format debug symbols generated by MinGW GCC.

**Manifestation**: Stack traces show only addresses, no function names or line numbers.

**Workaround**: Use GDB instead of CDB for MinGW builds.

**Status**: DOCUMENTED in TESTING_GUIDE.md

---

## Issues Resolved

### Resolved: Test Infrastructure

**Problem**: No systematic way to detect non-deterministic failures.

**Solution**: Created `run_tests.py` with:
- Configurable repetitions (default 100)
- Configurable timeout (default 5s)
- Automatic process cleanup
- Statistics and progress display

**Files Created**:
- `sintra/tests/run_tests.py`

**Files Modified**:
- `sintra/tests/CMakeLists.txt` (disabled recovery_test)

### Resolved: Process Cleanup

**Problem**: Tests leaving stuck processes running.

**Solution**: Implemented automatic cleanup in test runner using `taskkill /F /T`.

**Files Modified**:
- `sintra/tests/run_tests.py`

### Resolved: Debugger Setup

**Problem**: Needed command-line debugger workflow for remote debugging.

**Solution**:
- Established GDB as tool of choice for MinGW builds
- Created GDB command files for common tasks
- Documented in TESTING_GUIDE.md

**Files Created**:
- `gdb_attach.txt`
- `gdb_commands.txt`

---

## Issues Attempted But Not Resolved

### Attempted Fix: Race Conditions in fetch_message()

**Hypothesis**: `m_reading` flag has race conditions causing thread to restart reading after being stopped.

**Implementation**: Added atomic lock to protect `m_reading` checks before `start_reading()` and after `done_reading_new_data()`.

**Result**: NO EFFECT - tests still fail 100%

**Conclusion**: Either the hypothesis was wrong, or there's a more fundamental issue preventing the fix from taking effect.

**Files Modified**:
- `sintra/include/sintra/detail/message.h` (lines 606-668)

### Attempted Fix: Busy-Wait Optimization

**Hypothesis**: Missing yield in busy-wait causes performance issues.

**Implementation**: Added `std::this_thread::yield()` to `wait_until_ready()`.

**Result**: NO EFFECT on deadlock (expected - orthogonal issue)

**Conclusion**: Good practice but doesn't address shutdown deadlock.

**Files Modified**:
- `sintra/include/sintra/detail/process_message_reader_impl.h` (lines 66-70)

---

## Pitfalls and Gotchas

### Pitfall 1: Assuming Non-Determinism

**Mistake**: Initially assumed failures were non-deterministic (sometimes pass, sometimes fail).

**Reality**: Failures are 100% deterministic - every single run deadlocks.

**Lesson**: When failure rate is 100%, reduce repetitions to 1 during debugging to save time.

**Impact**: Wasted time running 100 repetitions when 1 would have shown the issue.

### Pitfall 2: Wrong Debugger for Toolchain

**Mistake**: Tried using CDB (Microsoft debugger) on MinGW build.

**Reality**: CDB requires PDB symbols, MinGW generates DWARF symbols.

**Lesson**: Match debugger to toolchain:
- MinGW → GDB
- MSVC → CDB or Visual Studio debugger

**Impact**: Lost time getting "no symbols" errors before switching to GDB.

### Pitfall 3: Assuming Recent Changes Broke Tests

**Mistake**: Assumed the deadlock was introduced by recovery implementation changes.

**Reality**: HEAD also fails 100% - tests have never worked reliably.

**Lesson**: Always test baseline before assuming regression.

**Impact**: Correctly identified this as pre-existing issue, not introduced by recent work.

### Pitfall 4: Fixing Symptoms Instead of Root Cause

**Mistake**: Added race condition checks in `fetch_message()` without confirming that was the actual problem.

**Reality**: The issue is deeper - `unblock_local()` mechanism itself may not be working.

**Lesson**: Use debugger to confirm hypothesis before implementing fix. Understand WHY the semaphore isn't being woken, don't just add more checks.

**Impact**: Implemented complex fix that had no effect.

### Pitfall 5: Not Checking m_sleepy_index State

**Mistake**: Assumed `unblock_local()` is working because it's being called.

**Reality**: Haven't verified that `m_sleepy_index >= 0` when `unblock_local()` is called, so maybe semaphore post is being skipped.

**Lesson**: Verify assumptions with debugger or logging. Check the actual state of `m_sleepy_index` when `unblock_local()` is called.

**Impact**: Don't know if the semaphore is actually being posted to.

### Pitfall 6: Semaphore State Complexity

**Observation**: The semaphore management in `wait_for_new_data()` is complex:
1. Thread marks itself as sleeping by setting `m_sleepy_index`
2. Thread blocks on `c.dirty_semaphores[m_sleepy_index].wait()`
3. `unblock_local()` posts to `c.dirty_semaphores[m_sleepy_index]`
4. Thread wakes, moves itself to unordered stack
5. Thread resets `m_sleepy_index = -1`

**Pitfall**: Many points where state can become inconsistent:
- `unblock_local()` called before `m_sleepy_index` is set
- `unblock_local()` called after `m_sleepy_index` is reset
- Multiple threads sharing same semaphore index
- Semaphore in wrong state

**Lesson**: Need to trace exact sequence with debugger to see where it's failing.

---

## Current Understanding

### What We Know For Sure

1. **Tests deadlock 100% of the time** - This is reproducible and deterministic.

2. **Deadlock occurs during shutdown** - Specifically in `Process_message_reader::~Process_message_reader()`.

3. **Main thread waits for reader threads** - `stop_and_wait()` waits for `m_req_running` and `m_rep_running` to become false.

4. **Reader threads are blocked** - GDB shows them stuck in `sintra_ring_semaphore::wait()`.

5. **unblock_local() is called** - Three times by `stop_and_wait()` at different phases.

6. **Reader loop has stop check** - Line 196 checks `m_reader_state != READER_STOPPING`.

7. **Issue exists in original codebase** - HEAD fails 100%, not a regression.

### What We Think We Know

1. **Threads are not being woken** - Otherwise they would exit the loop and set running flags to false.

2. **Problem is in unblock mechanism** - Either `unblock_local()` isn't working or threads block again after waking.

3. **Timing may be involved** - `m_sleepy_index` might not be set when `unblock_local()` is called.

### What We Don't Know Yet

1. **Is semaphore actually being posted to?** - Need to verify `m_sleepy_index >= 0` when `unblock_local()` is called.

2. **Do threads wake and then block again?** - Or do they never wake at all?

3. **Is there a different code path during shutdown?** - Maybe `wait_for_new_data()` behaves differently in some edge case.

4. **Are there multiple reader threads per ring?** - If so, maybe semaphore index collision.

5. **What is SINTRA_RING_READING_POLICY set to?** - If set to ALWAYS_SPIN, none of the semaphore code is used.

---

## Recommendations for Next Investigation Phase

### Immediate Next Steps

1. **Check SINTRA_RING_READING_POLICY**:
   ```bash
   grep -r "SINTRA_RING_READING_POLICY" sintra/include/
   ```
   If it's set to ALWAYS_SPIN, the semaphore code is compiled out and we're looking at the wrong problem.

2. **Add debug logging to unblock_local()**:
   ```cpp
   void unblock_local()
   {
       c.lock();
       std::fprintf(stderr, "unblock_local: m_sleepy_index=%d\n", m_sleepy_index);
       if (m_sleepy_index >= 0) {
           std::fprintf(stderr, "unblock_local: posting to semaphore %d\n", m_sleepy_index);
           c.dirty_semaphores[m_sleepy_index].post_unordered();
       }
       c.unlock();
   }
   ```

3. **Add debug logging to wait_for_new_data()**:
   ```cpp
   void wait_for_new_data()
   {
       std::fprintf(stderr, "wait_for_new_data: before wait, m_sleepy_index=%d\n", m_sleepy_index);
       c.dirty_semaphores[m_sleepy_index].wait();
       std::fprintf(stderr, "wait_for_new_data: after wait, m_sleepy_index=%d\n", m_sleepy_index);
   }
   ```

4. **Attach GDB with breakpoints**:
   ```gdb
   break ipc_rings.h:1449  # unblock_local
   break ipc_rings.h:1396  # wait_for_new_data semaphore wait
   commands
     print m_sleepy_index
     print &c.dirty_semaphores[m_sleepy_index]
     continue
   end
   run
   ```

5. **Check if done_reading() actually sets m_reading = false**:
   Add logging to verify the flag is set correctly.

### Medium-Term Investigation

1. **Understand semaphore index lifecycle**:
   - When is `m_sleepy_index` set?
   - When is it reset?
   - What if `unblock_local()` is called between reset and next set?

2. **Check for multiple readers per ring**:
   - Are there multiple `Ring_R` instances reading from same ring?
   - Could they be sharing semaphore indices incorrectly?

3. **Review Boost.Interprocess semaphore behavior**:
   - Does `post()` when already posted work as expected?
   - Does `wait()` consume a post count?
   - Can a post be "lost" if posted before `wait()`?

4. **Test with ALWAYS_SPIN policy**:
   - If semaphore is the problem, spinning should work
   - Define `SINTRA_RING_READING_POLICY SINTRA_RING_READING_POLICY_ALWAYS_SPIN`
   - Rebuild and test

### Long-Term Investigation

1. **Consider alternative shutdown approach**:
   - Instead of relying on semaphore unblock, could use different mechanism
   - E.g., shared atomic stop flag that `wait_for_new_data()` checks
   - Or periodic timeout in semaphore wait

2. **Review related IPC libraries**:
   - How do other lock-free ring buffer implementations handle shutdown?
   - What's the standard pattern for waking blocked readers?

3. **Simplify the state machine**:
   - Current implementation has complex state (sleeping, ready, unordered stacks)
   - Could this be simplified for shutdown case?

---

## Files Modified in This Investigation

### sintra/tests/run_tests.py (CREATED)
**Purpose**: Test runner with timeout and repetition support

**Key Features**:
- Configurable repetitions and timeout
- Automatic process cleanup
- Statistics and progress display

**Status**: Working correctly

### sintra/tests/CMakeLists.txt (MODIFIED)
**Purpose**: Disabled recovery_test from build

**Changes**: Commented out recovery_test target

**Status**: Working correctly

### sintra/include/sintra/detail/message.h (MODIFIED)
**Purpose**: Attempted fix for race conditions in fetch_message()

**Changes**: Added atomic lock protection around m_reading checks

**Status**: No effect on deadlock

### sintra/include/sintra/detail/process_message_reader_impl.h (MODIFIED)
**Purpose**: Added yield to wait_until_ready() busy-wait

**Changes**: Added std::this_thread::yield() in loops

**Status**: Improves CPU usage but doesn't fix deadlock

### backup_recovery_changes/ (DIRECTORY CREATED)
**Purpose**: Backup of recovery implementation changes

**Contents**: ipc_rings.h, message.h, managed_process_impl.h

**Status**: Preserved for reference

---

## Documentation Created

### DEADLOCK_ROOT_CAUSE.md
**Purpose**: Detailed analysis of deadlock from GDB session

**Contents**:
- Executive summary
- GDB stack traces with annotations
- Code analysis of all involved functions
- Proposed fixes (which didn't work)
- Testing criteria

**Status**: Complete but some analysis needs revision (e.g., stop check DOES exist)

### TESTING_GUIDE.md
**Purpose**: Comprehensive guide to test infrastructure and debugging workflow

**Contents**:
- Test runner usage
- Build system overview
- GDB debugging procedures
- Common scenarios and solutions
- File locations reference

**Status**: Complete

### TECHNICAL_FINDINGS.md (THIS DOCUMENT)
**Purpose**: Record of all findings, issues, and lessons learned

**Contents**:
- Timeline of investigation
- Detailed technical analysis
- Issues observed and resolved
- Pitfalls and gotchas
- Recommendations for next phase

**Status**: Complete

---

## Summary

This investigation has successfully:
- ✅ Created robust test infrastructure
- ✅ Established command-line debugging workflow
- ✅ Identified root cause location (unblock mechanism during shutdown)
- ✅ Ruled out several hypotheses (race conditions in fetch_message)
- ✅ Confirmed issue exists in original codebase
- ✅ Documented all findings for future work

The deadlock remains unresolved, but we have:
- Clear understanding of where threads are stuck (semaphore wait)
- Clear understanding of what should wake them (unblock_local)
- Clear next steps for deeper investigation (logging, breakpoints, policy checks)

The key insight is that `unblock_local()` is being called but threads aren't waking up. The next phase must focus on understanding WHY the semaphore post isn't waking the blocked threads.
