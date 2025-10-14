# Debugging Methodology for Sintra Multi-Process Tests

**Date**: 2025-10-14
**Context**: Testing framework for multi-process IPC tests

---

## Critical Distinction: Coordinator vs Worker Processes

### Process Architecture

When running Sintra tests like `sintra_rpc_append_test.exe`:

1. **Coordinator Process (Main)**:
   - PID reported by test runner (e.g., "Preserving PID 56924")
   - This is the PARENT process
   - Often exits quickly or is waiting, but NOT the one that hangs

2. **Worker Processes (Spawned)**:
   - Child processes spawned by the coordinator
   - These are the processes that actually execute test logic
   - **These are usually the ones that hang!**
   - Same binary name: `sintra_rpc_append_test.exe`
   - Spawned around the same time as coordinator

---

## Correct Debugging Workflow

### Step 1: Run Test with Process Preservation

```bash
cd C:/plms/imakris/sintra_repo/sintra/tests
python run_tests.py --build-dir ../../build --config Debug \
    --repetitions 1 --timeout 20 --test rpc_append \
    --preserve-stalled-processes
```

**Output Example**:
```
Process stalled (PID 56924). Preserving for debugging as requested.
Attach a debugger to PID 56924 or terminate it manually when done.
```

**⚠️ WARNING**: PID 56924 is the COORDINATOR, not necessarily the hung process!

---

### Step 2: Find All Running Worker Processes

**Use PowerShell** to find all processes with the test binary name:

```powershell
powershell -Command "Get-Process | Where-Object {$_.ProcessName -like '*sintra_rpc_append*'} | Select-Object Id,ProcessName,StartTime"
```

**Look for**:
- Multiple PIDs with the same binary name
- Processes started around the same time (within seconds)
- The worker PIDs will be DIFFERENT from the coordinator PID

**Example Output**:
```
Id      ProcessName               StartTime
--      -----------               ---------
56924   sintra_rpc_append_test    2025-10-14 10:15:30
50568   sintra_rpc_append_test    2025-10-14 10:15:31  ← WORKER!
50571   sintra_rpc_append_test    2025-10-14 10:15:31  ← WORKER!
```

---

### Step 3: Attach Debugger to WORKER Process

**Attach to a worker PID** (NOT the coordinator):

```bash
"C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe" -p 50568 -c "~*k; q"
```

**Why Worker Processes?**
- Workers execute the actual test logic (RPC calls, barriers, etc.)
- Coordinator may just be waiting for workers to complete
- Hangs/deadlocks typically occur in workers during:
  - RPC execution
  - Barrier synchronization
  - Shutdown/cleanup
  - Message handler execution

---

### Step 4: Capture Thread Stacks

**Command**:
```bash
"C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe" -p <WORKER_PID> -c "~*k; q" > worker_<WORKER_PID>_stack.txt 2>&1
```

**Analyze**:
- Look for threads waiting on mutexes
- Check for threads in message handlers
- Identify threads stuck in condition variable waits
- Look for Windows MessageBox dialogs (abort/assertion failures)

---

## Common Pitfalls

### ❌ WRONG: Attaching to Coordinator PID

```bash
# Test runner says: "Preserving PID 56924"
cdb.exe -p 56924  # ← WRONG! This is likely the coordinator
```

**Result**:
- "Cannot debug pid" error (already exited)
- OR coordinator is just waiting, not the actual problem

### ✅ CORRECT: Finding and Attaching to Workers

```powershell
# 1. Find all test processes
Get-Process | Where-Object {$_.ProcessName -like '*sintra_rpc_append*'}

# 2. Identify workers (spawned slightly after coordinator)

# 3. Attach to worker
cdb.exe -p 50568  # ← Worker PID
```

---

## Why This Matters

**Sintra Test Architecture**:
- Coordinator spawns N worker processes (typically 2-3)
- Workers communicate via shared memory and message queues
- Workers execute test logic (pub/sub, RPC, barriers)
- Deadlocks/hangs occur in workers during:
  - RPC shutdown (`ensure_rpc_shutdown()`)
  - Barrier waits (`sintra::barrier()`)
  - Message handler execution
  - Mutex/condition variable operations

**Debugging the coordinator gives you**:
- Waiting on worker processes to exit
- Not the actual deadlock location

**Debugging the workers gives you**:
- The ACTUAL hang location
- Thread stacks showing mutex waits
- Message handler state
- RPC lifecycle state

---

## Testing Methodology Summary

1. **Run test with `--preserve-stalled-processes`**
2. **Note the coordinator PID** (reported by test runner)
3. **Find ALL processes** with same binary name using PowerShell
4. **Identify worker PIDs** (spawned around same time as coordinator)
5. **Attach debugger to WORKER process**, not coordinator
6. **Capture thread stacks** from worker to identify hang location
7. **Kill all processes** when done: `taskkill //F //IM <binary>.exe`

---

## Example Session

```bash
# 1. Run test
python run_tests.py --test rpc_append --preserve-stalled-processes
# Output: "Preserving PID 56924"

# 2. Find workers
powershell -Command "Get-Process | Where-Object {$_.ProcessName -like '*sintra_rpc_append*'}"
# Output: PIDs 56924, 50568, 50571

# 3. Attach to worker (e.g., 50568)
"C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe" -p 50568 -c "~*k; q"

# 4. Analyze stack trace
# Look for mutex waits, handler threads, etc.

# 5. Clean up
taskkill //F //IM sintra_rpc_append_test.exe
```

---

## Key Insight

**The PID reported by `--preserve-stalled-processes` is the PARENT (coordinator).**

**The PIDs you want to debug are the CHILDREN (workers).**

**Always search for running processes with the same binary name to find the workers.**
