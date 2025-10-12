# Filesystem Cleanup Hang Investigation

**Date**: 2025-01-12
**Branch**: `declutter/windows-recovery-fix`
**Issue**: Tests hang during process cleanup in `remove_directory()` on Windows

---

## Problem Summary

After removing OpenMP dependency from tests (to fix OpenMP thread cleanup deadlock), a new underlying issue was revealed: tests now hang during filesystem cleanup when the `Managed_process` destructor calls `remove_directory()` to clean up the swarm directory.

**Key Observation**: As the user predicted earlier: *"this kind of issue is not related to openmp though. i am sure there is another underlying issue. we will see that later."* This prediction was accurate.

---

## Timeline of Discovery

### 1. Initial Symptoms
- **When**: After implementing OpenMP removal fix (commit e429d4f)
- **Manifestation**: Soak tests stall at random iterations (e.g., iteration 35, iteration 53)
- **Test Environment**: `python run_tests.py --repetitions 100 --timeout 10 --abort_on_stalled_processes`

### 2. First GDB Investigation (Process PID 85732)

**Command**:
```bash
"C:\Qt\Tools\mingw1310_64\bin\gdb.exe" -p 85732 -batch -x gdb_backtrace.txt
```

**Stack Trace**:
```
Thread 1 (Main Thread):
#0  ntdll!RtlDosPathNameToRelativeNtPathName_U
#1-2  KERNELBASE!FindFirstFileW (Windows filesystem API)
#3  msvcrt!_wstat64
#4-7  libstdc++-6.dll (C++ filesystem operations)
#8  sintra::remove_directory() at ipc_rings.h:399
#9  Managed_process::~Managed_process() at managed_process_impl.h:429
#10 sintra::finalize() at sintra_impl.h:162
#11 main() at basic_pub_sub.cpp:293
```

**Analysis**:
- Process hanging in Windows `FindFirstFileW` API call
- Called from `std::filesystem::remove_all()` during directory cleanup
- Occurs during `Managed_process` destructor execution
- Directory being cleaned: `C:\Users\imak\AppData\Local\Temp\/sintra/186da711d085f28c`

### 3. Second GDB Investigation (Process PID 56504)

After implementing retry logic fix, re-ran soak tests. Process stalled at iteration 53.

**Stack Trace** (identical location):
```
#0  ntdll!ZwClose
#1-2  KERNELBASE!FindFirstFileW
#3  msvcrt!_wstat64
#4-7  libstdc++-6.dll filesystem operations
#8  sintra::remove_directory() at ipc_rings.h:400
#9  Managed_process::~Managed_process() at managed_process_impl.h:429
#10 sintra::finalize() at sintra_impl.h:162
#11 main() at basic_pub_sub.cpp:293
```

**Critical Finding**: Hang occurs at **line 400** - the **first** call to `fs::remove_all()`, NOT in the retry loop.

---

## Root Cause Analysis

### Original Implementation (ipc_rings.h:396-401)

```cpp
/**
 * Remove a directory tree. Returns true if anything was removed.
 */
static inline bool remove_directory(const std::string& dir_name)
{
    std::error_code ec;
    auto removed = fs::remove_all(dir_name.c_str(), ec);
    return !!removed;
}
```

### Problem Identification

1. **Blocking Filesystem Call**: `std::filesystem::remove_all()` internally calls Windows `FindFirstFileW` to enumerate directory contents before deletion.

2. **Indefinite Block**: The Windows API call blocks indefinitely and never returns. This means:
   - No error code is ever set
   - No exception is thrown
   - Retry logic cannot execute (because the call never completes)

3. **Likely Causes**:
   - **File locks**: Child processes or memory-mapped files may still have handles open
   - **Race condition**: Concurrent test iterations may be accessing the same directory
   - **Windows filesystem delays**: Slow asynchronous cleanup operations
   - **Handle leaks**: File mappings (Ring_data) or control files (Ring) not fully released

### Similar Issue Fixed Previously

In `tests/basic_pub_sub.cpp` (lines 306-322), a similar filesystem race condition was fixed:

```cpp
// Retry cleanup with delays to handle filesystem race conditions on Ubuntu
bool cleanup_succeeded = false;
for (int retry = 0; retry < 3 && !cleanup_succeeded; ++retry) {
    try {
        std::filesystem::remove_all(shared_dir);
        cleanup_succeeded = true;
    }
    catch (const std::exception& e) {
        if (retry < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}
```

**Key Difference**: The test code retries after **exceptions** are caught. The library's `remove_directory()` never gets a chance to retry because it **never returns**.

---

## Attempted Fix

### Implementation (ipc_rings.h:393-419)

```cpp
/**
 * Remove a directory tree. Returns true if anything was removed.
 * Uses retry logic to handle filesystem delays on Windows.
 */
static inline bool remove_directory(const std::string& dir_name)
{
    std::error_code ec;
    auto removed = fs::remove_all(dir_name.c_str(), ec);

    // If the first attempt succeeded or the directory doesn't exist, we're done
    if (!ec || ec == std::errc::no_such_file_or_directory) {
        return !!removed;
    }

    // Retry up to 3 times with delays to handle filesystem locks/delays on Windows
    for (int retry = 0; retry < 3; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ec.clear();
        removed = fs::remove_all(dir_name.c_str(), ec);
        if (!ec || ec == std::errc::no_such_file_or_directory) {
            return !!removed;
        }
    }

    // After retries failed, return false (cleanup failed but don't crash)
    return false;
}
```

### Why This Fix Failed

**The retry logic is sound in principle, but cannot help in this scenario:**

1. The function blocks on the **first** `fs::remove_all()` call (line 400)
2. Execution never reaches the retry loop
3. The call never returns with an error code
4. The call never throws an exception
5. Therefore, no retry can be attempted

**GDB Evidence**: Second investigation showed hang at line 400, proving the first call never completes.

---

## Comparison with Previous Fixes

### 1. Filesystem Race in basic_pub_sub.cpp (RESOLVED)
- **Problem**: Directory collision when tests run in rapid succession
- **Root Cause**: Timestamp+PID uniqueness insufficient (< 1ms between tests)
- **Solution**: Added random 64-bit component to ensure uniqueness
- **Why it worked**: Prevented the same directory from being accessed concurrently

### 2. OpenMP Thread Cleanup Deadlock (RESOLVED)
- **Problem**: Tests hang during process exit in `libgomp-1.dll` cleanup
- **Root Cause**: OpenMP threads waiting during `LdrShutdownProcess`
- **Solution**: Removed OpenMP from tests, use chrono-based timing
- **Why it worked**: Eliminated the problematic library from test builds

### 3. Filesystem Cleanup Hang (CURRENT - UNRESOLVED)
- **Problem**: `FindFirstFileW` blocks indefinitely during directory deletion
- **Root Cause**: Unknown - likely file locks, handle leaks, or filesystem state
- **Attempted Solution**: Retry logic with delays
- **Why it failed**: Function call never returns, preventing retries

---

## Evidence and Debugging Sessions

### Critical Debugging Rule (User Instruction)
> **"when a process is hanging, you don't kill it. you connect to it with the debugger to see what is wrong. whenever you compact the session, keep this information. it is very very important"**

This rule was essential for discovering:
1. The exact line where blocking occurs (line 400)
2. The Windows API call responsible (`FindFirstFileW`)
3. The call stack showing destructor sequence
4. That retry logic never executes

### Soak Test Configuration
- **Repetitions**: 100 iterations per test
- **Timeout**: 10 seconds per iteration
- **Flag**: `--abort_on_stalled_processes` (leaves process running for GDB)
- **Failure Rate**: ~40-50% (varies by iteration count)

### Test That Passes 100%
- `sintra_ping_pong_test`: Always passes, no filesystem cleanup issues
- **Why?**: Unclear - possibly simpler cleanup path or shorter execution time

---

## Potential Root Causes (Hypothesis)

### 1. Memory-Mapped File Handles Still Open
**Location**: `Ring_data` destructor (ipc_rings.h:214-226)

```cpp
~Ring_data()
{
    delete m_data_region_0;
    delete m_data_region_1;
    m_data          = nullptr;

    if (m_remove_files_on_destruction) {
        std::error_code ec;
        (void)fs::remove(fs::path(m_data_filename), ec);
    }
}
```

**Concern**:
- Windows may keep file handles open briefly after `delete` of `ipc::mapped_region`
- Directory enumeration may fail if data files are still locked

### 2. Control File Handles Still Open
**Location**: `Ring` destructor (ipc_rings.h:691-705)

```cpp
~Ring()
{
    if (m_control->num_attached-- == 1) {
        this->m_remove_files_on_destruction = true;
    }

    delete m_control_region;
    m_control_region = nullptr;

    if (this->m_remove_files_on_destruction) {
        std::error_code ec;
        (void)fs::remove(fs::path(m_control_filename), ec);
    }
}
```

**Concern**:
- `m_control_region` deletion may not immediately release file locks
- Control file may still be mapped when directory cleanup starts

### 3. Destructor Ordering Issue
**Location**: `Managed_process::~Managed_process()` (managed_process_impl.h:394-447)

```cpp
~Managed_process()
{
    // ... cleanup readers ...
    m_readers.clear();

    // ... delete channels ...
    delete m_out_req_c;  // Message_ring_W - contains Ring_W
    delete m_out_rep_c;  // Message_ring_W - contains Ring_W

    if (s_coord) {
        delete s_coord;  // Coordinator - may contain multiple Rings
        s_coord = 0;

        // removes the swarm directory - THIS IS WHERE IT HANGS
        remove_directory(m_directory);  // line 429
    }
}
```

**Timeline**:
1. Readers cleared (may contain `Ring_R` objects)
2. Channels deleted (contain `Ring_W` objects)
3. Coordinator deleted (contains many Ring objects)
4. **Then** directory cleanup attempted

**Concern**:
- Destructor chain may not be fully complete
- Windows may still be releasing handles asynchronously
- No explicit delay between object deletion and directory cleanup

### 4. Concurrent Test Process Interference
**Observation**: Tests run in rapid succession (100 iterations, ~1-2 seconds each)

**Concern**:
- Previous test iteration's cleanup may still be in progress
- Windows filesystem operations are asynchronous
- New test iteration might start before old one fully cleans up
- Multiple processes might enumerate the same temp directory simultaneously

---

## Files Modified

### include/sintra/detail/ipc_rings.h
**Lines 393-419**: Added retry logic to `remove_directory()` function

**Changes**:
- Added retry loop with 3 attempts
- Added 100ms delay between retries
- Added error code checking and early return
- Added comments explaining Windows-specific issues
- Returns `false` on failure instead of potentially hanging

**Status**: Committed but **does not resolve the hang** (retry logic never executes)

---

## What Didn't Work

1. **Retry logic with delays**: Function blocks indefinitely on first call
2. **Error code checking**: Error codes never set when call blocks
3. **Graceful failure return**: Never reaches return statement

---

## What Needs Investigation

### 1. Handle Closure Guarantee
- How to ensure all `ipc::mapped_region` handles are fully closed?
- Does Windows release file locks synchronously or asynchronously?
- Should we add explicit delays after deleting mapped regions?

### 2. Directory Cleanup Necessity
- Is directory cleanup strictly necessary?
- Can we make it optional/best-effort?
- What happens if we skip cleanup on failure?

### 3. Cleanup Ordering
- Should we add delays between destructor steps?
- Should we explicitly flush/sync before directory removal?
- Is there a way to wait for Windows to release handles?

### 4. Alternative Cleanup Strategies
- Use `std::filesystem::remove()` on individual files instead of `remove_all()`?
- Spawn background thread for cleanup (non-blocking)?
- Use Windows-specific APIs with timeouts?

---

## Recommended Next Steps

### Option 1: Make Cleanup Optional
```cpp
static inline bool remove_directory(const std::string& dir_name)
{
    // Best-effort cleanup - don't block if filesystem is busy
    std::error_code ec;
    auto removed = fs::remove_all(dir_name.c_str(), ec);

    // Return success even if cleanup failed (temp dir will be cleaned by OS)
    return true;  // Changed from !!removed
}
```

**Pros**: Tests won't hang
**Cons**: Leaves temp directories behind (minor - OS cleans /tmp)

### Option 2: Add Explicit Delay Before Cleanup
```cpp
inline Managed_process::~Managed_process()
{
    // ... existing cleanup ...

    if (s_coord) {
        delete s_coord;
        s_coord = 0;

        // Give Windows time to release file handles
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        remove_directory(m_directory);
    }
}
```

**Pros**: May allow handles to be released
**Cons**: Adds delay to every test; may not fully solve issue

### Option 3: Background Cleanup Thread
```cpp
// Spawn detached thread for non-blocking cleanup
std::thread([dir = m_directory]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::error_code ec;
    fs::remove_all(dir, ec);
    // Ignore errors - best effort
}).detach();
```

**Pros**: Never blocks main thread
**Cons**: Cleanup not guaranteed; thread safety concerns

### Option 4: Investigate Handle Release
- Add instrumentation to track when `ipc::mapped_region` objects are destroyed
- Verify `num_attached` counter logic in `Ring` destructor
- Check if child processes are fully terminated before cleanup
- Add Windows-specific handle debugging

---

## Test Results

### Before Retry Logic Fix
- **Failure Rate**: ~40-50%
- **Failure Location**: `remove_directory()` line 399
- **Symptoms**: Indefinite hang in `FindFirstFileW`

### After Retry Logic Fix
- **Failure Rate**: ~40-50% (unchanged)
- **Failure Location**: `remove_directory()` line 400 (first call)
- **Symptoms**: Identical hang behavior
- **Conclusion**: Fix did not address root cause

---

## Conclusion

The retry logic implementation is **structurally correct** but **operationally ineffective** because it cannot execute when the underlying filesystem call blocks indefinitely. The real issue lies deeper in the handle management and cleanup sequence.

**The hang reveals a fundamental problem**: Something prevents Windows from enumerating the directory contents, likely because file handles are still open, either in the current process or potentially in recently-terminated child processes.

**Key Insight**: This is not a retry/timeout problem. This is a **resource cleanup ordering problem** or a **handle leak problem**.

Further investigation should focus on:
1. Ensuring all file handles are closed before directory cleanup
2. Adding explicit synchronization or delays
3. Making directory cleanup non-blocking or optional
4. Understanding Windows asynchronous file handle release behavior
