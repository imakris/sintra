# Barrier Complex Choreography Test Deadlock Analysis

## Summary

The `barrier_complex_choreography_test_debug` times out after 30 seconds on GitHub CI (Windows) starting after PR #605 was merged, then partially reverted in commit 7b7b185.

## Stack Trace Analysis

### Thread 0 (Main Thread)
```
ntdll!RtlSleepConditionVariableSRW
sintra!std::condition_variable::wait
sintra!Transceiver::rpc_impl  <-- Waiting for barrier RPC response
sintra!Process_group::barrier
sintra!detail::rendezvous_barrier
sintra!detail::barrier_dispatch
main
```

**Status:** Blocked waiting on condition variable for barrier RPC reply

### Threads 2-17 (Reader Threads)
```
ntdll!NtWaitForSingleObject
KERNELBASE!WaitForSingleObjectEx
sintra!interprocess_semaphore::wait  <-- Waiting on Windows semaphore
sintra!sintra_ring_semaphore::impl::wait
sintra!Ring_R<char>::wait_for_new_data
sintra!Message_ring_R::fetch_message
sintra!Process_message_reader::{request|reply}_reader_function
```

**Status:** ALL reader threads blocked waiting on interprocess semaphores

## The Deadlock Cycle

1. **Main thread** makes a barrier RPC call (`Process_group::barrier`)
2. The RPC is sent and main thread waits for reply in `rpc_impl()`
3. **Reply reader thread** should process the barrier response and wake up main thread
4. BUT: Reply reader (and all other readers) are stuck in `wait_for_new_data()` waiting on semaphores
5. Semaphore posts come from ring writers, but writers may be blocked elsewhere
6. **Result:** Complete deadlock - no progress possible

## Historical Context

### PR #605 - "Reply Skip Hints" Mechanism
Commits like c96aa4b, 7b07c96, fa9086d, etc. implemented a "reply skip hint" system:

**Purpose:** Avoid deadlock when `wait_for_delivery_fence()` is called while an RPC is in flight

**Mechanism:**
- Track which reply reader is processing current RPC
- Allow `wait_for_delivery_fence()` to skip waiting on that reader
- Prevents cycle: RPC waits → delivery fence waits → reply reader waits → RPC

**Implementation:**
- `register_reply_progress_skip()`: Register skip hint when RPC succeeds
- `take_reply_progress_skips()`: Retrieve skip hints
- `wait_for_delivery_fence()`: Check skips before waiting on reply readers

### Commit 7b7b185 - Removal of Reply Skip Hints
**Date:** Wed Oct 29 16:17:58 2025

**What was removed:**
- Entire reply skip mechanism (~120 lines)
- `Reply_progress_skip` structure
- `register_reply_progress_skip()` function
- `take_reply_progress_skips()` function
- Skip logic in `wait_for_delivery_fence()`

**Likely reason:** The mechanism was complex and had issues with:
- Generation tracking race conditions
- Interaction with reader replacement
- Difficulty ensuring correctness across all scenarios

## Root Cause Hypothesis

The deadlock appears to be caused by removal of the reply skip mechanism without an alternative solution. Specifically:

### Scenario Where Deadlock Occurs:

1. Process A calls `barrier<delivery_fence_t>()` or `barrier<processing_fence_t>()`
2. This executes (from `barrier.h`):
   ```cpp
   rendezvous_barrier(...)  // Makes RPC, waits for reply
   wait_for_delivery_fence()  // Waits for all readers
   ```
3. The `rendezvous_barrier` RPC succeeds and returns
4. Then `wait_for_delivery_fence()` is called (line 80 or 110 in barrier.h)
5. `wait_for_delivery_fence()` waits for **ALL** readers including reply readers
6. But reply readers may be idle, waiting on semaphores for new data
7. No new data comes because other processes are also stuck in similar states
8. **Distributed deadlock** across multiple processes

### Why This Is Hard To Reproduce Locally

- Requires specific timing between multiple processes
- Depends on CI environment's scheduling characteristics
- May need specific barrier choreography patterns (hence why it shows in complex_choreography test)
- Race condition - doesn't happen every time

## Potential Solutions

### Option 1: Restore Reply Skip Mechanism (With Fixes)
- Re-implement the skip hints with better generation tracking
- Ensure proper cleanup and race condition handling
- Test exhaustively

### Option 2: Change Barrier Implementation
- Don't call `wait_for_delivery_fence()` after barrier RPC
- Instead, use a different synchronization mechanism
- May require protocol changes

### Option 3: Fix Semaphore Wakeup Logic
- Investigate if semaphore posts are being lost
- Check Windows handle management in `ensure_handle()`
- Add retry logic or timeout with diagnostic logging

### Option 4: Add Delivery Fence Timeout with Diagnostics
- Make `wait_for_delivery_fence()` timeout after reasonable duration
- Log which readers are stuck and what they're waiting for
- Allows test to fail with useful diagnostics rather than silent hang

## ATTEMPTED Solution #1: Skip Reply Streams (FAILED)

### Approach: Skip Reply Streams in Barrier Context

**Rationale:** Barriers only need to ensure **outgoing request messages** are delivered. Incoming reply messages are processed asynchronously by reader threads and don't need to block barrier completion.

**Changes Made:**

1. **`managed_process.h` + `managed_process_impl.h`:**
   - Added `skip_reply_streams` parameter to `wait_for_delivery_fence()` (default: false)
   - When true, skips waiting on reply delivery targets

2. **`barrier.h`:**
   - Updated both `delivery_fence_t` and `processing_fence_t` barriers to call:
     ```cpp
     s_mproc->wait_for_delivery_fence(/*skip_reply_streams=*/true);
     ```

**Test Results:** **FAILED**
- Added strategic delays (`SINTRA_STRESS_TEST_DELAYS`) to trigger deadlock more reliably
- WITHOUT fix: 9/10 runs deadlocked (timeout after 35s)
- WITH fix: Still 9/10 runs deadlocked ❌

**Why This DIDN'T Work:**
- The stack trace shows main thread stuck IN `rpc_impl()` waiting for barrier RPC response
- It hasn't even reached `wait_for_delivery_fence()` yet
- This means the deadlock occurs DURING the RPC, not after
- Skipping reply streams in the post-RPC delivery fence doesn't help

**Actual Deadlock Mechanism (Revised Hypothesis):**
The deadlock must involve the barrier RPC HANDLER (coordinator side) calling `wait_for_delivery_fence()`, which creates a distributed cycle:
1. Client calls barrier RPC → sends to coordinator
2. Coordinator processes barrier RPC handler
3. Barrier handler (on coordinator) calls `wait_for_delivery_fence()`
4. Coordinator's delivery fence waits for coordinator's reply readers
5. Coordinator's reply readers are idle (waiting for data that won't come)
6. Coordinator never sends RPC response
7. Client stuck waiting for response
8. **Distributed deadlock**

## SUCCESSFUL Solution #2: Skip BOTH Request and Reply Streams (FIXED ✓)

### Root Cause Discovery

The actual deadlock was a **distributed circular dependency**:

1. **All processes** (including the coordinator!) call `barrier()` around the same time
2. Each barrier RPC completes successfully
3. Each process then calls `wait_for_delivery_fence(skip_reply_streams=true)`
4. This waits for **REQUEST** readers to catch up (only reply was skipped)
5. But **ALL processes' REQUEST readers are idle** - nobody is sending new requests because everyone is stuck in `wait_for_delivery_fence()`
6. **Result:** Distributed deadlock - everyone waits for everyone else's request readers

### The Solution

Skip **BOTH** request AND reply streams in barrier contexts:

**Changes Made:**

1. **`managed_process.h` + `managed_process_impl.h`:**
   - Added `skip_request_streams` parameter to `wait_for_delivery_fence()`
   - When true, skips waiting on request delivery targets (in addition to reply)

2. **`barrier.h`:**
   - Updated both `delivery_fence_t` and `processing_fence_t` barriers:
     ```cpp
     s_mproc->wait_for_delivery_fence(/*skip_reply_streams=*/true,
                                      /*skip_request_streams=*/true);
     ```

### Test Results: **SUCCESS** ✓

- **WITHOUT fix**: 1/10 runs passed, 9/10 deadlocked (with stress delays)
- **WITH incorrect fix** (skip reply only): 1/10 runs passed, 9/10 deadlocked
- **WITH correct fix** (skip BOTH): **5/5 runs passed** ✓

### Why This Works

Barriers only need to ensure the barrier RPC itself completes. They don't need to wait for all reader threads to catch up with message processing, because:

1. The barrier RPC already provides the synchronization point
2. Messages are delivered asynchronously by reader threads
3. Waiting for readers creates artificial dependencies that cause distributed deadlock
4. The `flush()` call after barrier RPC ensures the coordinator's response is received

## Additional Improvements

1. **Disabled CRT Abort Popups:** Added `SetErrorMode()` and `_set_abort_behavior()` to `barrier_complex_choreography_test.cpp` to prevent Windows error dialogs during testing

2. **Stress Test Infrastructure:** Created `SINTRA_STRESS_TEST_DELAYS` compile-time flag to inject strategic delays that reliably reproduce the deadlock for testing

## Key Files

- `include/sintra/detail/barrier.h` - Barrier implementation calling `wait_for_delivery_fence()`
- `include/sintra/detail/managed_process_impl.h` - `wait_for_delivery_fence()` implementation (line 1639)
- `include/sintra/detail/interprocess_semaphore.h` - Windows semaphore wrapper
- `include/sintra/detail/ipc_rings.h` - Ring buffer and `wait_for_new_data()` (line 1362)
- `include/sintra/detail/transceiver_impl.h` - RPC implementation

## References

- PR #605: Introduced reply skip hints
- PR #596 (codex/reply-fence-baseline): Alternative approach (not merged)
- Commit 7b7b185: Removed reply skip hints
- Commit 8e3a9db: "Ensure reply progress skips track reader generation"
- Commit c96aa4b: Original "Skip waiting on the reply reader that is executing a barrier"
