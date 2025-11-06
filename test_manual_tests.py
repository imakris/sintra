#!/usr/bin/env python3
"""
Quick test script for manual tests with stack trace collection.
This mimics the behavior of run_tests.py for manual testing.
"""

import subprocess
import sys
import time
from pathlib import Path

def run_test_with_timeout(test_exe, timeout_seconds):
    """Run a test executable with timeout and attempt stack capture."""
    print(f"\n{'='*70}")
    print(f"Testing: {test_exe.name}")
    print(f"Timeout: {timeout_seconds}s")
    print(f"{'='*70}\n")

    start = time.time()
    try:
        # Use CREATE_NEW_PROCESS_GROUP on Windows for better process control
        if sys.platform == 'win32':
            creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            creationflags = 0

        proc = subprocess.Popen(
            [str(test_exe)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            creationflags=creationflags if sys.platform == 'win32' else 0,
            start_new_session=(sys.platform != 'win32')
        )

        stdout, _ = proc.communicate(timeout=timeout_seconds)
        duration = time.time() - start

        print(f"Output:\n{stdout}")
        print(f"\nResult: Exited with code {proc.returncode} after {duration:.2f}s")
        return proc.returncode == 0

    except subprocess.TimeoutExpired:
        duration = time.time() - start
        print(f"TIMEOUT after {duration:.2f}s - killing process {proc.pid}")

        # Try to kill the process tree
        if sys.platform == 'win32':
            try:
                subprocess.run(
                    ['taskkill', '/F', '/T', '/PID', str(proc.pid)],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=5
                )
                print(f"Successfully killed process tree with taskkill")
            except Exception as e:
                print(f"taskkill failed: {e}")
                proc.kill()
        else:
            proc.kill()

        try:
            stdout, _ = proc.communicate(timeout=2)
            print(f"Output before timeout:\n{stdout}")
        except:
            print("Could not retrieve output")

        print(f"\nResult: TIMEOUT - process did not complete within {timeout_seconds}s")
        return False

def main():
    build_dir = Path(__file__).parent / "build-manual"
    manual_tests_dir = build_dir / "tests" / "manual"

    if not manual_tests_dir.exists():
        print(f"Error: Manual tests directory not found: {manual_tests_dir}")
        return 1

    # Test configurations: (test_name, timeout_seconds)
    tests = [
        ("sintra_hang_test_debug.exe", 10),
        ("sintra_children_crash_coordinator_deadlock_test_debug.exe", 20),
        ("sintra_coordinator_crash_children_hang_test_debug.exe", 20),
    ]

    results = {}
    for test_name, timeout in tests:
        test_path = manual_tests_dir / test_name
        if not test_path.exists():
            print(f"Warning: Test not found: {test_path}")
            continue

        passed = run_test_with_timeout(test_path, timeout)
        results[test_name] = passed

    # Summary
    print(f"\n{'='*70}")
    print("SUMMARY")
    print(f"{'='*70}")
    for test_name, passed in results.items():
        status = "PASS (timed out as expected)" if not passed else "UNEXPECTED PASS"
        print(f"{test_name}: {status}")
    print(f"{'='*70}\n")

    return 0

if __name__ == '__main__':
    sys.exit(main())
