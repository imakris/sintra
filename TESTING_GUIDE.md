# Sintra Testing Guide

## Overview

This guide explains how to test the Sintra IPC library, including the test infrastructure, workflow for debugging test failures, and common pitfalls.

## Test Infrastructure

### Test Runner Script

**Location**: `tests/run_tests.py`

The test runner is a Python script that executes all Sintra tests with configurable timeouts and repetitions. It's designed to catch non-deterministic failures caused by OS scheduling issues.

**Key Features**:
- Configurable number of repetitions (default: 100)
- Configurable timeout per test run (default: 5 seconds)
- Automatic cleanup of stuck processes using `taskkill /F /T`
- Pre-cleanup of all sintra processes before starting
- Progress visualization and detailed statistics
- Colored output for pass/fail/timeout results

**Usage**:
```bash
# Run with defaults (100 repetitions, 5s timeout)
python run_tests.py

# Run with custom settings
python run_tests.py --repetitions 10 --timeout 2.0

# Run a single iteration for quick testing
python run_tests.py --repetitions 1 --timeout 30

# Specify build directory and configuration
python run_tests.py --build-dir ../build-ninja2 --config Debug
```

**Command-line Arguments**:
- `--build-dir`: Path to CMake build directory (default: `../build-ninja2`)
- `--config`: Build configuration - Debug or Release (default: `Debug`)
- `--repetitions`: Number of times to run each test (default: `100`)
- `--timeout`: Timeout in seconds for each test run (default: `5.0`)

### Why Repetitions Matter

Sintra is an inter-process communication library where timing-dependent bugs are common due to:
- OS thread scheduling variations
- Semaphore wake-up timing
- Race conditions in shutdown sequences

A test might pass 100 times but deadlock on the 101st run due to rare scheduling conditions. Therefore:
- **Development**: Use 100+ repetitions to verify fixes
- **CI**: May need lower repetitions for time constraints (discuss trade-offs)
- **Debugging**: Use 1 repetition when failure rate is 100%

### Process Cleanup

The test runner automatically handles stuck processes:

1. **Pre-cleanup**: Kills all `sintra_*_test.exe` processes before starting
2. **Timeout handling**: When a test times out, kills the entire process tree using `taskkill /F /T`
3. **Manual cleanup**: If needed, kill all tests manually:
   ```bash
   taskkill /F /IM sintra_basic_pubsub_test.exe /T
   taskkill /F /IM sintra_rpc_test.exe /T
   taskkill /F /IM sintra_typed_test.exe /T
   taskkill /F /IM sintra_instance_test.exe /T
   ```

## Build System

### Directory Structure

```
sintra_repo/
├── sintra/
│   ├── include/sintra/         # Header-only library
│   │   └── detail/             # Implementation headers
│   └── tests/                  # Test source files
│       ├── basic_pub_sub.cpp
│       ├── rpc_test.cpp
│       ├── typed_test.cpp
│       ├── instance_test.cpp
│       ├── recovery_test.cpp   # Currently disabled
│       ├── CMakeLists.txt
│       └── run_tests.py
├── build-ninja2/               # CMake build directory
│   └── tests/
│       ├── sintra_basic_pubsub_test.exe
│       ├── sintra_rpc_test.exe
│       ├── sintra_typed_test.exe
│       └── sintra_instance_test.exe
└── backup_recovery_changes/    # Backup of recovery implementation
```

### Building Tests

**Prerequisites**:
- CMake 3.15+
- MinGW GCC (for GDB debug support)
- Boost.Interprocess library

**Build Commands**:
```bash
# From sintra_repo root
cd sintra

# Configure CMake with Ninja (recommended)
cmake -G Ninja -S . -B ../build-ninja2 -DCMAKE_BUILD_TYPE=Debug

# Build all tests
cmake --build ../build-ninja2

# Or build a specific test
cmake --build ../build-ninja2 --target sintra_basic_pubsub_test
```

**Important**: Always use **Debug** builds for testing and debugging. Debug builds include:
- DWARF format debug symbols (`-g`)
- No optimization (`-O0`)
- Assertions enabled

### Disabling Tests

To disable a test from the build (e.g., recovery_test which has unimplemented features):

Edit `tests/CMakeLists.txt` and comment out the test:
```cmake
# Recovery test disabled - feature not yet fully implemented
# add_executable(sintra_recovery_test recovery_test.cpp)
# target_link_libraries(sintra_recovery_test PRIVATE sintra)
# add_test(NAME sintra.recovery COMMAND sintra_recovery_test)
```

Then rebuild:
```bash
cmake --build ../build-ninja2
```

## Debugging Test Failures

### Quick Debugging Workflow

When a test fails:

1. **Reduce iterations**: If failure rate is 100%, use `--repetitions 1` to save time
2. **Increase timeout**: Use `--timeout 30` or `--timeout 60` to distinguish deadlocks from slow tests
3. **Run specific test**: Navigate to build directory and run test manually
4. **Attach debugger**: Use GDB to get stack traces (see below)

### Using GDB (Recommended for MinGW builds)

**Prerequisites**:
- MinGW GDB: `C:\Qt\Tools\mingw1310_64\bin\gdb.exe` (or your MinGW installation)
- Debug build with DWARF symbols

**Verify Symbols**:
```bash
cd C:\plms\imakris\sintra_repo\build-ninja2\tests
"C:\Qt\Tools\mingw1310_64\bin\gdb.exe" --batch -ex "file sintra_basic_pubsub_test.exe" -ex "info functions wait_until"
```

You should see function names and line numbers. If not, rebuild with Debug configuration.

### Method 1: Attach to Hung Process

When a test hangs, attach GDB to get stack traces:

1. **Find the process ID**:
   ```bash
   tasklist | findstr sintra_basic_pubsub_test
   ```
   Output: `sintra_basic_pubsub_test.exe    12345 Console    1    15,432 K`

2. **Create GDB command file** (`gdb_attach.txt`):
   ```
   set pagination off
   set confirm off
   info threads
   thread apply all bt
   quit
   ```

3. **Attach GDB**:
   ```bash
   "C:\Qt\Tools\mingw1310_64\bin\gdb.exe" -p 12345 -x gdb_attach.txt > gdb_output.txt
   ```

4. **Analyze stack traces** in `gdb_output.txt`

### Method 2: Run Test Under GDB

1. **Start test in background**:
   ```bash
   cd C:\plms\imakris\sintra_repo\build-ninja2\tests
   start /B sintra_basic_pubsub_test.exe
   ```

2. **Immediately attach GDB** (while test is running):
   ```bash
   # Get PID quickly
   for /f "tokens=2" %i in ('tasklist /fi "imagename eq sintra_basic_pubsub_test.exe" /fo list ^| findstr /i "PID:"') do set PID=%i

   # Attach
   "C:\Qt\Tools\mingw1310_64\bin\gdb.exe" -p %PID% -x gdb_attach.txt
   ```

### Method 3: Interactive GDB Session

For detailed debugging:

```bash
cd C:\plms\imakris\sintra_repo\build-ninja2\tests
"C:\Qt\Tools\mingw1310_64\bin\gdb.exe" sintra_basic_pubsub_test.exe
```

**Useful GDB commands**:
```gdb
(gdb) run                          # Start the program
(gdb) info threads                 # List all threads
(gdb) thread 2                     # Switch to thread 2
(gdb) bt                           # Backtrace for current thread
(gdb) thread apply all bt          # Backtrace for all threads
(gdb) frame 5                      # Switch to stack frame 5
(gdb) info locals                  # Show local variables
(gdb) print m_reader_state         # Print variable value
(gdb) break process_message_reader_impl.h:213  # Set breakpoint
(gdb) continue                     # Continue execution
(gdb) quit                         # Exit GDB
```

### CDB (Windows Debugger Kit)

**Location**: `C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe`

**Important Limitation**: CDB requires PDB (Microsoft) format debug symbols. MinGW GCC generates DWARF format, so CDB cannot read symbols from MinGW builds.

**To use CDB**: Build with Microsoft Visual C++ compiler instead of MinGW, or convert symbols (complex).

**Recommendation**: Use GDB for MinGW builds. Only use CDB if building with MSVC.

## Interpreting Test Results

### Success Output
```
Running 100 repetitions of 4 tests with 5.0s timeout...

sintra_basic_pubsub_test: [====================] 100/100 (5.23s avg)
  PASS: 100 | FAIL: 0 | TIMEOUT: 0
sintra_rpc_test: [====================] 100/100 (4.87s avg)
  PASS: 100 | FAIL: 0 | TIMEOUT: 0
...

SUCCESS RATE: 100.00% (400/400 passed)
```

### Partial Failure Output
```
sintra_basic_pubsub_test: [====================] 100/100 (11.23s avg)
  PASS: 92 | FAIL: 0 | TIMEOUT: 8
```

This indicates:
- 8% of runs deadlocked (hit 5s timeout)
- Non-deterministic failure - timing-dependent bug
- Need more repetitions to characterize failure rate
- Likely race condition or deadlock during shutdown

### Complete Failure Output
```
sintra_basic_pubsub_test: [====================] 100/100 (5.01s avg)
  PASS: 0 | FAIL: 0 | TIMEOUT: 100

SUCCESS RATE: 0.00% (0/400 passed)
```

This indicates:
- 100% failure rate - deterministic bug
- All tests deadlock at same point
- Use `--repetitions 1` to save time during debugging
- Attach GDB to understand root cause

## Common Debugging Scenarios

### Scenario 1: Test Deadlocks 100% of the Time

**Symptoms**: All repetitions timeout, no passes

**Debugging Steps**:
1. Switch to single iteration:
   ```bash
   python run_tests.py --repetitions 1 --timeout 30
   ```

2. Run test manually and attach GDB:
   ```bash
   cd ../build-ninja2/tests
   start /B sintra_basic_pubsub_test.exe
   # Get PID from tasklist
   "C:\Qt\Tools\mingw1310_64\bin\gdb.exe" -p <PID> -x gdb_attach.txt > gdb_output.txt
   ```

3. Analyze all thread stacks in `gdb_output.txt`
   - Look for threads blocked in semaphore waits
   - Look for threads waiting on condition variables
   - Identify which synchronization primitive is stuck

4. Review code at identified stack frames
   - Check shutdown logic
   - Check if stop flags are being set and checked
   - Check if unblock mechanisms are being called

### Scenario 2: Test Occasionally Fails (8-20% failure rate)

**Symptoms**: Most runs pass, some timeout

**Debugging Steps**:
1. Increase repetitions to characterize failure:
   ```bash
   python run_tests.py --repetitions 500
   ```

2. Try different timeout values to see if it's just slow:
   ```bash
   python run_tests.py --repetitions 100 --timeout 10
   ```

3. Add logging/instrumentation to suspect code paths

4. Run under debugger with breakpoints on rare paths

### Scenario 3: GDB Shows No Symbols

**Symptoms**: Stack traces show addresses but no function names or line numbers

**Cause**: Missing debug symbols or wrong debugger for build toolchain

**Solution**:
1. Verify Debug build:
   ```bash
   cmake -G Ninja -S . -B ../build-ninja2 -DCMAKE_BUILD_TYPE=Debug
   cmake --build ../build-ninja2
   ```

2. Check symbols exist:
   ```bash
   "C:\Qt\Tools\mingw1310_64\bin\gdb.exe" --batch -ex "file sintra_basic_pubsub_test.exe" -ex "info functions"
   ```

3. If using CDB with MinGW build, switch to GDB
4. If using GDB with MSVC build, switch to CDB

## Test-Specific Notes

### basic_pub_sub.cpp
Tests basic publish-subscribe messaging between processes.

**Key points**:
- Creates 2 processes
- Sends events between them
- Tests shutdown sequence

**Common failures**: Deadlock in `finalize()` during shutdown

### rpc_test.cpp
Tests remote procedure calls between processes.

**Key points**:
- Tests synchronous RPC calls
- Tests exception handling
- Tests return values

**Common failures**: Deadlock waiting for RPC reply

### typed_test.cpp
Tests type-safe messaging.

**Key points**:
- Tests template-based message types
- Tests type registration

### instance_test.cpp
Tests instance management and routing.

**Key points**:
- Tests multiple instances per process
- Tests message routing to specific instances

### recovery_test.cpp (DISABLED)

**Status**: Currently disabled in CMakeLists.txt

**Reason**: Tests features not yet fully implemented (process crash recovery)

**To re-enable**: Uncomment lines in `tests/CMakeLists.txt` after implementing recovery features

## Performance Considerations

### Test Timeouts

Choosing the right timeout value:

- **Too short** (< 2s): May cause false failures on slow systems or under load
- **Too long** (> 30s): Wastes time when debugging 100% failures
- **Recommended**:
  - Development: 5s (catches deadlocks quickly)
  - Debugging 100% failures: 30-60s (to distinguish slow from hung)
  - CI: 10s (balance between reliability and speed)

### Number of Repetitions

- **Quick validation**: 10-20 repetitions
- **Confidence testing**: 100 repetitions
- **Stress testing**: 500-1000 repetitions
- **Continuous Integration**: 50-100 repetitions (time constraints)

### Build Performance

- Ninja is faster than Visual Studio generator
- Parallel builds: `cmake --build ../build-ninja2 -j 8`
- Ccache can speed up rebuilds (if installed)

## Workflow for Future Sessions

When starting a new debugging session:

1. **Understand current state**:
   ```bash
   git status
   git log --oneline -10
   ```

2. **Build tests**:
   ```bash
   cd sintra
   cmake --build ../build-ninja2
   ```

3. **Run quick validation**:
   ```bash
   cd tests
   python run_tests.py --repetitions 10
   ```

4. **If failures occur**:
   - Check `DEADLOCK_ROOT_CAUSE.md` for known issues
   - Check `TECHNICAL_FINDINGS.md` for debugging context
   - Run with single iteration and attach GDB

5. **After making changes**:
   ```bash
   cmake --build ../build-ninja2
   python run_tests.py --repetitions 100
   ```

6. **Before committing**:
   ```bash
   # Ensure tests pass reliably
   python run_tests.py --repetitions 500

   # Document findings
   # Update TECHNICAL_FINDINGS.md if needed

   # Commit
   git add <files>
   git commit -m "descriptive message"
   ```

## Troubleshooting

### Problem: "Python not found"
**Solution**: Ensure Python 3.6+ is in PATH

### Problem: Tests immediately fail with "Access denied"
**Solution**: Kill existing sintra test processes:
```bash
taskkill /F /IM sintra_basic_pubsub_test.exe /T
```

### Problem: GDB error 87 (access denied)
**Solution**: Run command prompt as Administrator, or start test in background and attach immediately

### Problem: Build fails with missing Boost headers
**Solution**: Install Boost or set `BOOST_ROOT` environment variable

### Problem: Ninja not found
**Solution**: Install Ninja or use different generator:
```bash
cmake -G "MinGW Makefiles" -S . -B ../build-mingw -DCMAKE_BUILD_TYPE=Debug
```

## Additional Resources

- **Boost.Interprocess Documentation**: https://www.boost.org/doc/libs/release/doc/html/interprocess.html
- **GDB Documentation**: https://sourceware.org/gdb/documentation/
- **CMake Documentation**: https://cmake.org/documentation/

## File Locations Reference

- Test source: `sintra_repo/sintra/tests/*.cpp`
- Test runner: `sintra_repo/sintra/tests/run_tests.py`
- Test executables: `sintra_repo/build-ninja2/tests/*.exe`
- Library headers: `sintra_repo/sintra/include/sintra/**/*.h`
- Debug symbols: Embedded in `.exe` files (DWARF format)
- Root cause analysis: `sintra_repo/DEADLOCK_ROOT_CAUSE.md`
- Technical findings: `sintra_repo/TECHNICAL_FINDINGS.md`
- This guide: `sintra_repo/TESTING_GUIDE.md`
