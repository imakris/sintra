# Shutdown Deadlock Fix - Progress Report

## Problem Statement

All Sintra tests were deadlocking 100% of the time during shutdown in `Process_message_reader::~Process_message_reader()` → `stop_and_wait()`.

**Root Cause**: Race condition between `unblock_local()` and `wait_for_new_data()`:
- Reader thread temporarily resets `m_sleepy_index = -1` before checking for data
- Main thread calls `stop_nowait()` → `done_reading()` → `unblock_local()`
- `unblock_local()` checks `if (m_sleepy_index >= 0)` → **FALSE** (it's -1), so no semaphore post
- Reader thread then sets `m_sleepy_index` to valid value and blocks on semaphore
- **No one will ever wake the reader** - the unblock already happened when index was -1

## Solution Implemented

Added an explicit shutdown flag (`m_stopping`) to `Ring_R` class that is checked at multiple points before blocking.

### Changes Made

**File**: `sintra/include/sintra/detail/ipc_rings.h`

**1. Added shutdown flag** (line 1472):
```cpp
private:
    int                                 m_sleepy_index          = -1;
    int                                 m_rs_index              = -1;
    std::atomic<bool>                   m_stopping              = false;  // ← NEW
```

**2. Modified `done_reading()`** to set flag and unblock when called without active snapshot (lines 1367-1372):
```cpp
else {
    // done_reading() called without active snapshot => shutdown signal
    m_stopping.store(true, std::memory_order_release);
    // Wake the reader thread if it's blocked in wait_for_new_data()
    unblock_local();
}
```

**3. Modified `wait_for_new_data()`** to check flag at critical points (lines 1388-1442):
- Before blocking (line 1389-1391)
- During spin phase for ALWAYS_SPIN policy (line 1396-1398)
- During hybrid spin phase (line 1406-1408)
- Before registering as sleeping (line 1417-1420)
- After waking from semaphore (line 1440-1442)

## Test Results

### Before Fix
```
Pass Rate: 0.00% (0/400 passed)
All tests: TIMEOUT
```

### After Initial Flag Implementation
```
Pass Rate: 30.00% (120/400 passed)
basic_pubsub_test: 30.0% pass rate
```

### After Adding unblock_local() Call
```
Pass Rate: 55.00% (220/400 passed)
basic_pubsub_test: 55.0% pass rate
avg duration: 2.55s (down from 5.00s timeout)
```

## Progress Analysis

**Improvement**: 0% → 55% (dramatic improvement, but not complete)

### Why Not 100%?

The `unblock_local()` call in `done_reading()` still depends on `m_sleepy_index` being set:

```cpp
void unblock_local()
{
    c.lock();
    if (m_sleepy_index >= 0) {  // ← Still checking this
        c.dirty_semaphores[m_sleepy_index].post_unordered();
    }
    c.unlock();
}
```

**Remaining race window** (~45% of cases):
1. Thread reaches `wait_for_new_data()`, enters HYBRID spin phase
2. Spin timeout expires, thread locks control, sets `m_sleepy_index = -1`
3. Thread checks for data (line 1415): `if (m_reading_sequence->load() == c.leading_sequence.load())`
4. **Shutdown happens here**: Main thread sets `m_stopping = true`, calls `unblock_local()`
5. `unblock_local()` sees `m_sleepy_index = -1`, does nothing
6. Reader thread continues (missed the flag check at line 1417 due to timing)
7. Reader sets `m_sleepy_index = c.ready_stack[--c.num_ready]` (line 1421)
8. Reader unlocks control, blocks on semaphore (line 1427)
9. **Deadlock** - semaphore was never posted

**Why 55% works**: The flag checks at lines 1389, 1396, 1406, 1417, 1440 catch most cases where shutdown happens during waiting. But the narrow window between line 1415 and line 1421 (while holding the lock) is not protected by a flag check.

## Next Steps to Achieve 100%

### Option A: Add flag check while holding lock (RECOMMENDED)
```cpp
c.lock();
m_sleepy_index = -1;
if (m_reading_sequence->load() == c.leading_sequence.load()) {
    // Check for shutdown AFTER checking for data, BEFORE setting sleepy_index
    if (m_stopping.load(std::memory_order_acquire)) {
        c.unlock();
        return Range<T>{};
    }
    m_sleepy_index = c.ready_stack[--c.num_ready];
    c.sleeping_stack[c.num_sleeping++] = m_sleepy_index;
}
c.unlock();
```

**Pros**: Closes the race window completely
**Cons**: None - this is the correct fix

### Option B: Use global unblock mechanism
Post to ALL sleeping semaphores instead of just this reader's:
```cpp
void done_reading()
{
    // ...
    else {
        m_stopping.store(true, std::memory_order_release);
        // Post to ALL sleeping readers
        c.lock();
        for (int i = 0; i < c.num_sleeping; i++) {
            c.dirty_semaphores[c.sleeping_stack[i]].post_ordered();
        }
        c.unlock();
    }
}
```

**Pros**: Guaranteed to wake the thread
**Cons**: Wakes other readers too (but they're from same process, so OK)

### Option C: Retry unblock with delay
```cpp
unblock_local();
std::this_thread::sleep_for(std::chrono::milliseconds(1));
unblock_local();  // Try again in case index was set
```

**Pros**: Simple
**Cons**: Unreliable, still has race window, adds delay

## Recommendation

Implement **Option A** - add the flag check at line 1417 (which already exists in current code!). This should bring us to 100% pass rate.

## Current Status

- ✅ Identified root cause via GDB debugging
- ✅ Implemented shutdown flag mechanism
- ✅ Achieved 55% pass rate (up from 0%)
- ⏳ Need to close remaining race window for 100%

## Performance Impact

**Minimal**: The `m_stopping.load()` checks add negligible overhead:
- Only checked when no data is available (not the hot path)
- Atomic load with acquire semantics is very fast (~few CPU cycles)
- No contention since flag is write-once during shutdown

## Files Modified

- `sintra/include/sintra/detail/ipc_rings.h`
  - Line 1472: Added `m_stopping` flag
  - Lines 1367-1372: Modified `done_reading()` to set flag and unblock
  - Lines 1388-1442: Modified `wait_for_new_data()` to check flag

## Testing

To verify the fix:
```bash
cd sintra/tests
python run_tests.py --repetitions 100 --timeout 5
```

Current results: 55% pass rate
Target: 100% pass rate after implementing Option A
