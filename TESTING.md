# Sintra Testing Guide

This document describes how to build and run tests for the Sintra library.

## Table of Contents

- [Quick Start](#quick-start)
- [Test Selection: active_tests.txt](#test-selection-active_teststxt)
- [Building Tests](#building-tests)
- [Running Tests](#running-tests)
- [Common Workflows](#common-workflows)
- [Understanding Test Iterations](#understanding-test-iterations)
- [Manual Tests](#manual-tests)
- [Test Runner Options](#test-runner-options)
- [CI/CD Integration](#cicd-integration)

## Quick Start

```bash
# 1. Configure and build test binaries
cmake -B build -DSINTRA_BUILD_TESTS=ON
cmake --build build

# 2. Run tests
cd tests
python3 run_tests.py --build-dir ../build --config Release
```

## Test Selection: active_tests.txt

Sintra uses `tests/active_tests.txt` to control which built tests are run and how often.

### Format

Each non-comment line specifies a test and its iteration count:

```
test_name iterations
```

- `test_name`: Test path relative to `tests/` directory (without `.cpp` extension)
- `iterations`: Number of times to run the test (must be ≥ 1)
- Lines starting with `#` are treated as comments and ignored
- Empty lines are ignored

### Example

```
# Core functionality tests
dummy_test 1
ping_pong_test 200
basic_pub_sub 30

# Stress tests
barrier_stress_test 10
recovery_test 10

# Manual tests (commented out by default)
# manual/some_problematic_test 10
```

### How It Works

Build and run selection are separate:

**CMake (Build Phase)**
- Builds the available test binaries
- Builds manual tests only when `SINTRA_BUILD_MANUAL_TESTS=ON`

**run_tests.py (Run Phase)**
- Reads `active_tests.txt` at startup
- Discovers built test binaries matching the selected tests
- Runs each test for the specified number of iterations

This split means:
- CMake does not duplicate the `active_tests.txt` parser
- Commented tests are not run
- Iteration counts are versioned with the code
- Easy to focus on specific tests during debugging

## Building Tests

### Standard Build

```bash
cmake -B build -DSINTRA_BUILD_TESTS=ON
cmake --build build
```

Tests are built in two configurations:
- **Debug** (`_debug` suffix): `-O0 -g` for debugging
- **Release** (`_release` suffix): `-O3 -DNDEBUG` for performance testing

### Build Only Specific Tests

Edit `tests/active_tests.txt` and comment out tests you don't need:

```bash
# Comment out all tests except the one you're working on
# barrier_complex_choreography_test 1
# barrier_flush_test 20
ping_pong_test 5    # Only this will be built
# ...rest commented out...
```

Then rebuild:

```bash
cmake --build build
```

CMake will skip building the commented-out tests.

### Build with Debug Symbols in Release Mode

```bash
cmake -B build -DSINTRA_BUILD_TESTS=ON -DSINTRA_RELEASE_WITH_DEBUG_SYMBOLS=ON
cmake --build build
```

This enables debugging of optimized code.

## Running Tests

### Basic Usage

```bash
cd tests
python3 run_tests.py --build-dir ../build --config Release
```

### Run Debug Configuration

```bash
python3 run_tests.py --build-dir ../build --config Debug
```

### Multi-process teardown helpers

Tests that end with the ordinary "all participants reach the same top-level
shutdown point" pattern should prefer
`sintra::test::run_multi_process_shutdown_test(...)` from `tests/test_utils.h`.
That helper runs the coordinator action, then calls `sintra::shutdown()` in
every process.

Keep using `run_multi_process_test(...)` when a test needs a more specific
final protocol before teardown, such as an explicit final rendezvous inside the
test logic or an abnormal-exit path that cannot participate in symmetric
shutdown.

### Change Iteration Counts

Edit the iteration counts directly in `active_tests.txt`:

```bash
# To run ping_pong_test more times, edit tests/active_tests.txt:
ping_pong_test 5000   # Increase from default

# Then run tests
python3 run_tests.py --build-dir ../build --config Release
```

### Scale Iteration Counts

Use an iteration multiplier to scale the whole suite without editing
`active_tests.txt`. A multiplier of `1` preserves the checked-in counts. Values
below `1` reduce repetitions, with every active test still running at least
once.

```bash
python3 run_tests.py --iteration-multiplier 0.5 --build-dir ../build --config Release
```

The same setting can be supplied through `SINTRA_TEST_ITERATION_MULTIPLIER`.

### Extended Timeout

Some tests (like `recovery_test`) need more time:

```bash
python3 run_tests.py --timeout 30 --build-dir ../build --config Release
```

### Verbose Output

See detailed output from each test run:

```bash
python3 run_tests.py --verbose --build-dir ../build --config Release
```

## Common Workflows

### Debugging a Failing Test

1. **Isolate the test** - Edit `tests/active_tests.txt`:
   ```
   # Comment out everything except the failing test
   problematic_test 1
   ```

2. **Rebuild** (only builds the one test):
   ```bash
   cmake --build build
   ```

3. **Run with verbose output**:
   ```bash
   cd tests
   python3 run_tests.py --verbose --config Debug --build-dir ../build
   ```

4. **Run under debugger** (if needed):
   ```bash
   gdb ../build/tests/sintra_problematic_test_debug
   ```

5. **Restore all tests** when done - Uncomment tests in `active_tests.txt`

### Running a Stress Test

1. **Edit iteration count** in `active_tests.txt`:
   ```
   ping_pong_test 1000
   ```

2. **Run the test**:
   ```bash
   cd tests
   python3 run_tests.py --config Release --build-dir ../build
   ```

### Quick Sanity Check

Just run `dummy_test` to verify the infrastructure works:

1. **Edit `active_tests.txt`**:
   ```
   dummy_test 1
   ```

2. **Build and run**:
   ```bash
   cmake --build build
   cd tests && python3 run_tests.py --build-dir ../build --config Release
   ```

## Understanding Test Iterations

The number of iterations for each test is specified directly in `active_tests.txt`.

### Iteration Strategy

The iteration counts in `active_tests.txt` reflect:

- **Flakiness detection**: Tests with non-deterministic behavior (e.g., `ping_pong_test: 200`) run many times to catch race conditions
- **Basic validation**: Simple tests (e.g., `dummy_test: 1`) run once per configuration
- **Stress testing**: Tests that exercise specific edge cases run repeatedly

### Special Test Handling

**ipc_rings_tests**

This test executable contains multiple sub-tests. The runner automatically discovers and expands them:

```
ipc_rings_tests 1
```

Becomes:
- `ipc_rings_tests_release:unit:test_ring_write_read_single_reader`
- `ipc_rings_tests_release:unit:test_multiple_readers_see_same_data`
- `ipc_rings_tests_release:stress:stress_attach_detach_readers`
- ... (and more)

Each sub-test inherits the iteration count from the parent entry.

### Test Timeouts

Default timeout is 5 seconds per test run. Some tests override this:

- `recovery_test`: 120 seconds (tests crash recovery, which is slow)

Override globally with `--timeout`:

```bash
python3 run_tests.py --timeout 60 --build-dir ../build --config Release
```

## Manual Tests

The `tests/manual/` directory contains:
- Reproduction tests for specific bugs
- Tests that intentionally fail (for infrastructure testing)
- Experimental or one-off diagnostics

These are **commented out by default** in `active_tests.txt`:

```
# manual/barrier_delivery_fence_repro_test 1
# manual/guard_pending_writer_integration_test 1
```

### Enabling Manual Tests

1. **Uncomment the test** in `active_tests.txt`:
   ```
   manual/barrier_delivery_fence_repro_test 10
   ```

2. **Configure with manual tests enabled, then rebuild**:
   ```bash
   cmake -B build -DSINTRA_BUILD_TESTS=ON -DSINTRA_BUILD_MANUAL_TESTS=ON
   cmake --build build
   ```

3. **Run**:
   ```bash
   cd tests
   python3 run_tests.py --build-dir ../build --config Debug
   ```

Manual tests are only built when `SINTRA_BUILD_MANUAL_TESTS=ON`.

## Test Runner Options

### Full Option List

```
python3 run_tests.py [OPTIONS]

Options:
  --timeout SECONDS     Timeout per test run in seconds (default: 5.0)
  --build-dir PATH      Path to build directory (default: ../build-ninja2)
  --config CONFIG       Build configuration: Debug or Release (default: Debug)
  --verbose             Show detailed output for each test run
  --preserve-stalled-processes
                        Keep stalled processes for debugging instead of killing them
  --iteration-multiplier VALUE
                        Scale active_tests.txt repetition counts; every active
                        test still runs at least once
```

### Configuration

The runner automatically detects your build directory structure:
- Looks for `build-dir/tests/` (flat structure)
- Falls back to `build-dir/tests/Release/` or `build-dir/tests/Debug/` (MSVC-style)

### Output

The runner displays:
- Git branch and revision
- Number of active tests
- Test plan table with iteration counts
- Progress indicators (`.` = pass, `F` = fail)
- Summary with pass/fail counts and timing statistics

Example output:
```
Sintra Test Runner
Git branch: main
Git revision: 2a9011e
Build directory: /home/user/sintra/build
Configuration: Release
Timeout per test: 5.0s
Active tests: 26 tests from active_tests.txt
======================================================================

Found 2 configuration suite(s) to run

================================================================================
Configuration 1/2: debug
  Tests in suite: 26
================================================================================

  Test overview
  ==============================================
  ID  Name                          Repetitions
  ==============================================
  01  dummy_test                              1
  02  ping_pong_test                        200
  03  basic_pub_sub                          30
  ...
```

## CI/CD Integration

The test infrastructure is used by CI workflows on multiple platforms.

### GitHub Actions

```yaml
- name: Run Tests
  run: |
    cd tests
    python run_tests.py --timeout 30 --build-dir ../build --config Release
```

### GitHub Actions (FreeBSD via QEMU)

FreeBSD runs on a GitHub Actions Ubuntu runner inside a QEMU VM
(`vmactions/freebsd-vm`):

```yaml
- name: Build and test (FreeBSD 15.0)
  uses: vmactions/freebsd-vm@v1
  env:
    SINTRA_TEST_ITERATION_MULTIPLIER: "0.5"
  with:
    release: "15.0"
    usesh: true
    envs: 'SINTRA_TEST_ITERATION_MULTIPLIER'
    prepare: |
      pkg install -y cmake python
    run: |
      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
      cmake --build build
      cd tests
      python3 run_tests.py --timeout 30 --build-dir ../build --config Release
```

### Test Matrix Control

CI runs use the same `active_tests.txt` as local development, ensuring:
- Consistent test coverage across platforms
- Easy adjustment of test scope (comment out flaky tests temporarily)
- Single source of truth for all environments

### Build-Only CI

For quick build validation without tests:

```bash
cmake -B build -DSINTRA_BUILD_TESTS=OFF
cmake --build build
```

## Troubleshooting

### "No tests found in active_tests.txt"

- Check that `tests/active_tests.txt` exists
- Ensure at least one test line is not commented out
- Verify the file format (no syntax errors)

### "Test 'foo' from active_tests.txt not found in build directory"

- The test is listed in `active_tests.txt` but wasn't built
- Run `cmake --build build` to build tests
- Check that the test name matches the source file (without `.cpp`)

### Tests Timing Out

- Increase timeout: `--timeout 60`
- Check for deadlocks or infinite loops
- Verify system resources (memory, CPU)
- Some tests legitimately need longer (e.g., `recovery_test`)

### Lifeline behavior in tests

- `lifeline_basic_test` covers normal exit, disabled lifeline, and missing lifeline paths.
- Spawned processes require a lifeline by default; if you launch a test binary
  manually, pass `--lifeline_disable` for that process or launch it via
  `spawn_swarm_process`.

### Tests Pass Locally but Fail in CI

- CI runs with higher iteration counts (specified in `active_tests.txt`)
- CI has different timing/scheduling behavior
- Check for race conditions or non-deterministic failures
- Increase local iteration counts in `active_tests.txt` to reproduce

### Build is Slow

- Comment out tests you're not working on in `active_tests.txt`
- CMake will skip building them
- Remember to uncomment before committing

## Migration from Old System

If you're used to the old test selection flags, here's the mapping:

| Old Command | New Approach |
|-------------|--------------|
| `--test ping_pong` | Edit `active_tests.txt`, comment out other tests |
| `--include "*stress*"` | Edit `active_tests.txt`, comment out non-stress tests |
| `--exclude recovery` | Comment out `recovery_test` in `active_tests.txt` |
| `--test-dir manual` | Configure with `-DSINTRA_BUILD_MANUAL_TESTS=ON`, then uncomment `manual/*` tests |
| `-DBUILD_MANUAL_TESTS=ON` | `-DSINTRA_BUILD_MANUAL_TESTS=ON` |

The runner selection lives in one file; CMake just builds binaries.
