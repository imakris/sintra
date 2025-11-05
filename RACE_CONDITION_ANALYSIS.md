# Race Condition Analysis: stress_multi_reader_throughput on macOS

## Problem Statement

The `stress_multi_reader_throughput` test fails intermittently on macOS with the following symptoms:
- Readers observe non-monotonic sequences (e.g., [5, 6, 7, 5, 8, 9] or [5, 5, 6, 7])
- Failure at assertion: `ASSERT_GT(results[i], results[i - 1])` in line 715
- Occurs despite PR #726's attempt to fix with busy guard timeout mechanism

## Root Cause Analysis

The bug is in the `mark_guard_busy()` function within `wait_for_new_data()` in `rings.h` (lines 1440-1463).

### The Race Condition

**Vulnerable code path (lines 1449-1451):**
```cpp
if (observed & Ring<T, false>::READER_GUARD_BUSY) {
    slot.guard_busy_since.store(now_us, std::memory_order_release);
    return true;  // ❌ BUG: Returns without checking if guard is still held!
}
```

**Race scenario:**
1. Thread A (reader): Line 1441: Loads `observed = READER_GUARD_HELD | READER_GUARD_BUSY`
2. Thread B (writer): Evicts reader, clears guard (sets to 0)
3. Thread A: Line 1449: Checks `observed & READER_GUARD_BUSY` - true (using stale cached value)
4. Thread A: Line 1450: Updates timestamp
5. Thread A: Line 1451: Returns true ❌ **BUG: Reader thinks it's still busy but was actually evicted!**
6. Thread A: Lines 1470-1472: Calculates range and advances sequence based on potentially stale state
7. Thread A: Returns range that might point to overwritten data
8. Reader copies stale/duplicate values into results array

### Why It Manifests on macOS (ARM) But Not Linux (x86-64)

**ARM (Apple Silicon):**
- Weaker memory ordering model
- Loads and stores can be reordered more aggressively
- CPU caches can remain stale longer
- The window for the race is much wider

**x86-64 (Linux):**
- Stronger memory ordering model (TSO - Total Store Order)
- Loads see stores more quickly
- Smaller race window makes it extremely rare

## The Fix

The fix is to always verify the guard is still held before trusting the busy flag:

```cpp
if (observed & Ring<T, false>::READER_GUARD_BUSY) {
    // ✅ Must re-check guard is still held, not just trust cached value
    uint8_t current = slot.has_guard.load(std::memory_order_acquire);
    if ((current & Ring<T, false>::READER_GUARD_HELD) == 0) {
        return false;  // Guard was cleared, we're evicted
    }
    slot.guard_busy_since.store(now_us, std::memory_order_release);
    return true;
}
```

## New Test: stress_multi_reader_throughput_aggressive

To help diagnose and reproduce this issue, a new test has been added that:

1. **Uses a very small ring buffer** (64 elements) - increases wrapping frequency
2. **Uses many readers** (8 instead of 3) - increases contention
3. **Adds artificial delays** (50µs) in readers - widens the race window
4. **Uses memory fences** (`std::atomic_thread_fence(std::memory_order_seq_cst)`) - forces visibility
5. **Provides detailed diagnostics** when the race is detected

### Expected Behavior

- **On macOS:** Should fail more frequently than the original test, clearly showing the race
- **On Linux:** May still pass due to stronger memory model, but will fail if the bug exists and race is hit
- **After fix:** Should pass consistently on both platforms

## Testing Recommendations

1. Run the new `stress_multi_reader_throughput_aggressive` test on macOS repeatedly (e.g., 100 iterations)
2. Apply the proposed fix to `rings.h`
3. Verify both original and aggressive tests pass consistently
4. Consider adding compiler barriers or using ThreadSanitizer for additional validation

## Related Issues

- PR #726: "Stabilize busy guard eviction on weakly ordered CPUs" - attempted to fix but incomplete
- PR #727: "Remove redundant code and simplify overcomplicated implementations"

## Summary

The busy guard mechanism introduced in PR #726 has a subtle but critical flaw: it trusts a cached guard status without re-validating it's still held. On weakly-ordered CPUs (ARM/macOS), this allows a reader to proceed with operations after being evicted, leading to non-monotonic data sequences. The fix requires adding a fresh memory_order_acquire load to verify the guard before trusting the busy flag.
