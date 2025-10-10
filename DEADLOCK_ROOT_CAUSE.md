# Sintra Deadlock - Root Cause & Solution

## Executive Summary

**Problem**: All tests deadlock in `finalize()` when destroying `Process_message_reader` objects.

**Root Cause**: Reader threads are blocked waiting for messages. When `finalize()` tries to destroy them, it waits for them to finish, but they never do because they're stuck in `wait_for_new_data()`.

**Solution**: Reader threads must check a "stopping" flag and exit gracefully when `stop_and_wait()` is called.

---

## Detailed Analysis from GDB

### Main Thread Stack (Thread 1) - DEADLOCKED

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

### Reader Thread Stack (Thread 5) - BLOCKED ON SEMAPHORE

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

---

## The Problem Flow

### Normal Shutdown Sequence (Expected)

1. Test finishes executing
2. Calls `sintra::finalize()`
3. `Managed_process` destructor runs
4. Calls `stop_and_wait()` on each `Process_message_reader`
5. **Reader threads should detect stop signal and exit**
6. `stop_and_wait()` succeeds
7. Cleanup completes

### Actual Broken Sequence

1. Test finishes executing
2. Calls `sintra::finalize()`
3. `Managed_process` destructor runs
4. Calls `stop_and_wait()` on each `Process_message_reader`
5. **Reader threads are stuck in `wait_for_new_data()` and NEVER CHECK stop flag**
6. `stop_and_wait()` times out after 22 seconds ← **DEADLOCK**
7. Never completes

---

## Code Analysis

### Location 1: `stop_and_wait()` Implementation

**File**: `include/sintra/detail/process_message_reader_impl.h:99-140`

```cpp
inline
bool Process_message_reader::stop_and_wait(double waiting_period)
{
    m_reader_state = READER_STOPPING;  // ← Sets stop flag

    // Wake up the reader threads by posting to semaphores
#if SINTRA_RING_READING_POLICY != SINTRA_RING_READING_POLICY_ALWAYS_SPIN
    m_in_req_c->unblock_local();
    m_in_rep_c->unblock_local();
#endif

    auto start = std::chrono::steady_clock::now();

    // Wait for request reader to finish
    {
        std::unique_lock<std::mutex> lock(m_stop_mutex);
        auto timeout = std::chrono::duration<double>(waiting_period);

        m_stop_condition.wait_for(lock, timeout, [this] {
            return !m_req_running;  // ← WAITING FOR THIS TO BECOME FALSE
        });
    }

    // Similar wait for reply reader...

    return !m_req_running && !m_rep_running;
}
```

**Key Points**:
- Sets `m_reader_state = READER_STOPPING`
- Calls `unblock_local()` to wake semaphores
- Waits up to 22 seconds for `m_req_running` to become false
- **Problem**: Reader threads never check `m_reader_state` and never set `m_req_running = false`

### Location 2: Reader Thread Function (THE BUG)

**File**: `include/sintra/detail/process_message_reader_impl.h:188-234`

```cpp
inline
void Process_message_reader::request_reader_function()
{
    m_req_running = true;  // ← Set on startup

    while (true)
    {
        // ❌ BUG: NO CHECK FOR m_reader_state HERE!

        // This blocks indefinitely waiting for messages
        auto msg = m_in_req_c->fetch_message();  // ← BLOCKS HERE

        if (!msg) {
            break;  // Only exits if fetch_message returns nullptr
        }

        // Process message...
    }

    // ❌ BUG: This code is NEVER REACHED because fetch_message() never returns!
    {
        std::lock_guard<std::mutex> lock(m_stop_mutex);
        m_req_running = false;  // ← SHOULD set this before exiting
        m_stop_condition.notify_one();  // ← SHOULD notify
    }
}
```

**The Bug**:
1. Reader thread calls `fetch_message()`
2. `fetch_message()` calls `wait_for_new_data()`
3. `wait_for_new_data()` calls semaphore `wait()` - **BLOCKS FOREVER**
4. Never checks if `m_reader_state == READER_STOPPING`
5. Never exits loop
6. Never sets `m_req_running = false`
7. Never notifies condition variable

### Location 3: `fetch_message()` - Where It Blocks

**File**: `include/sintra/detail/message.h:617-646`

```cpp
Message_prefix* fetch_message()
{
    if (m_range.begin == m_range.end)
    {
        if (m_range.begin)
            done_reading();

        // ❌ NO CHECK FOR STOPPING FLAG BEFORE BLOCKING!
        wait_for_new_data();  // ← BLOCKS ON SEMAPHORE

        m_range = get_available_range_for_reading();
    }

    Message_prefix* ret = (Message_prefix*)m_range.begin;
    assert(ret->magic == message_magic);

    m_range.begin += ret->size;
    return ret;
}
```

### Location 4: `wait_for_new_data()` - The Actual Block

**File**: `include/sintra/detail/ipc_rings.h:1393-1400`

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

**Note**: `unblock_local()` is called by `stop_and_wait()`, which SHOULD wake this semaphore, but the thread needs to check if it should exit after waking!

---

## The Fix

### Required Changes

#### Fix #1: Check Stop Flag in Reader Loop

**File**: `include/sintra/detail/process_message_reader_impl.h:188-234`

**Before**:
```cpp
void Process_message_reader::request_reader_function()
{
    m_req_running = true;

    while (true)
    {
        auto msg = m_in_req_c->fetch_message();  // Blocks forever

        if (!msg) {
            break;
        }

        // Process message...
    }

    {
        std::lock_guard<std::mutex> lock(m_stop_mutex);
        m_req_running = false;
        m_stop_condition.notify_one();
    }
}
```

**After**:
```cpp
void Process_message_reader::request_reader_function()
{
    m_req_running = true;

    while (m_reader_state != READER_STOPPING)  // ← CHECK STOP FLAG
    {
        auto msg = m_in_req_c->fetch_message();

        if (!msg) {
            break;
        }

        // Check again after blocking operation
        if (m_reader_state == READER_STOPPING) {
            break;
        }

        // Process message...
    }

    {
        std::lock_guard<std::mutex> lock(m_stop_mutex);
        m_req_running = false;
        m_stop_condition.notify_one();
    }
}
```

#### Fix #2: Make `fetch_message()` Respect Stop Flag

**File**: `include/sintra/detail/message.h:617-646`

**Option A**: Return `nullptr` when stopping

Add parameter to `Process_message_reader` so `fetch_message()` can check stop state:

```cpp
Message_prefix* fetch_message()
{
    if (m_range.begin == m_range.end)
    {
        if (m_range.begin)
            done_reading();

        wait_for_new_data();

        // Check if we were woken by unblock_local() due to stopping
        if (/* reader is stopping */) {
            return nullptr;  // Signal to exit
        }

        m_range = get_available_range_for_reading();
    }

    if (m_range.begin == m_range.end) {
        return nullptr;  // No data available
    }

    Message_prefix* ret = (Message_prefix*)m_range.begin;
    assert(ret->magic == message_magic);

    m_range.begin += ret->size;
    return ret;
}
```

**Option B**: Add explicit check after `wait_for_new_data()`

The simpler approach is to check in the reader loop itself (Fix #1 above), since `unblock_local()` will wake the semaphore and the loop can then check `m_reader_state`.

---

## Testing the Fix

### Before Fix
```bash
$ python run_tests.py --repetitions 5 --timeout 30
All 5 tests TIMEOUT (100% failure rate)
```

### After Fix
```bash
$ python run_tests.py --repetitions 100 --timeout 5
Expected: High pass rate (>95%)
```

---

## Implementation Plan

### Phase 1: Minimal Fix (Recommended First)

1. **Modify reader loop** to check `m_reader_state` after `fetch_message()` returns
2. **Test** with existing test suite
3. **Verify** no more deadlocks

### Phase 2: Robust Fix (If Needed)

1. **Add state parameter** to `Message_ring_R`
2. **Make `fetch_message()`** check stop flag and return `nullptr`
3. **Update all callers** to handle `nullptr` properly

### Phase 3: Add Busy-Wait Yield (Performance)

1. **Fix `wait_until_ready()`** to add `std::this_thread::yield()`
2. **Prevents** CPU spinning during initialization

---

## Files to Modify

### Priority 1 (Critical for deadlock fix):
- `include/sintra/detail/process_message_reader_impl.h:188-234` - Add stop check in reader loop
- `include/sintra/detail/process_message_reader_impl.h:236-280` - Same for reply reader

### Priority 2 (Performance improvement):
- `include/sintra/detail/process_message_reader_impl.h:66-69` - Add yield to `wait_until_ready()`

### Priority 3 (Cleaner solution):
- `include/sintra/detail/message.h:617-646` - Make `fetch_message()` stop-aware
- `include/sintra/detail/process_message_reader.h` - Add state accessor to reader

---

## Related Code Locations

### Reader State Management
- `include/sintra/detail/process_message_reader.h:77-82` - State enum definition
- `include/sintra/detail/process_message_reader.h:139` - `m_reader_state` member

### Semaphore Unblocking
- `include/sintra/detail/ipc_rings.h:1430-1464` - `unblock_local()` implementation
- Called by `stop_and_wait()` to wake blocked readers

### Test Code
- `tests/basic_pub_sub.cpp:265` - Calls `finalize()` which triggers deadlock
- All tests have same pattern

---

## Success Criteria

✅ **Fix is successful when:**
1. `stop_and_wait()` returns within timeout (< 22 seconds)
2. Reader threads exit cleanly when `m_reader_state = READER_STOPPING`
3. `m_req_running` and `m_rep_running` become `false`
4. `m_stop_condition` is notified
5. All tests pass with `python run_tests.py --repetitions 100`

---

## Next Steps

1. ✅ **Identified root cause** - Reader threads don't check stop flag
2. ✅ **Located exact code** - `process_message_reader_impl.h:188-234`
3. ⏳ **Implement fix** - Add `m_reader_state` check in reader loop
4. ⏳ **Test fix** - Run with test runner
5. ⏳ **Verify** - Ensure 100% pass rate
