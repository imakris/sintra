#!/usr/bin/env python3
"""
Sintra Test Runner with Timeout and Repetition Support

This script runs Sintra tests multiple times with proper timeout handling
to detect non-deterministic failures caused by OS scheduling issues.

Usage:
    python run_tests.py [options]

Options:
    --repetitions N                 Number of times to run each test (default: 100)
    --timeout SECONDS               Timeout per test run in seconds (default: 5)
    --test NAME                     Run only specific test (e.g., sintra_ping_pong_test)
    --build-dir PATH                Path to build directory (default: ../build-ninja2)
    --config CONFIG                 Build configuration Debug/Release (default: Debug)
    --verbose                       Show detailed output for each test run
    --preserve-stalled-processes    Keep stalled processes running for debugging (default: terminate)
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Tuple, Optional

class Color:
    """ANSI color codes for terminal output"""
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    RESET = '\033[0m'
    BOLD = '\033[1m'

def format_duration(seconds: float) -> str:
    """Format duration in human-readable format"""
    if seconds < 1:
        return f"{seconds*1000:.0f}ms"
    return f"{seconds:.2f}s"

class TestResult:
    """Result of a single test run"""
    def __init__(self, success: bool, duration: float, output: str = "", error: str = ""):
        self.success = success
        self.duration = duration
        self.output = output
        self.error = error

class TestRunner:
    """Manages test execution with timeout and repetition"""

    def __init__(self, build_dir: Path, config: str, timeout: float, verbose: bool,
                 preserve_on_timeout: bool = False):
        self.build_dir = build_dir
        self.config = config
        self.timeout = timeout
        self.verbose = verbose
        self.preserve_on_timeout = preserve_on_timeout

        # Determine test directory - check both with and without config subdirectory
        test_dir_with_config = build_dir / 'tests' / config
        test_dir_simple = build_dir / 'tests'

        if test_dir_with_config.exists():
            self.test_dir = test_dir_with_config
        else:
            self.test_dir = test_dir_simple

        # Kill any existing sintra processes for a clean start
        self._kill_all_sintra_processes()

    def find_tests(self, test_name: Optional[str] = None) -> List[Path]:
        """Find all test executables"""
        if not self.test_dir.exists():
            print(f"{Color.RED}Test directory not found: {self.test_dir}{Color.RESET}")
            return []

        # List of test executables to run
        test_names = [
            'sintra_dummy_test',
            'sintra_hanging_test',
        ]

        if test_name:
            test_names = [name for name in test_names if test_name in name]

        tests = []
        for name in test_names:
            if sys.platform == 'win32':
                test_path = self.test_dir / f"{name}.exe"
            else:
                test_path = self.test_dir / name

            if test_path.exists():
                tests.append(test_path)
            else:
                print(f"{Color.YELLOW}Warning: Test not found: {test_path}{Color.RESET}")

        return tests

    def run_test_once(self, test_path: Path) -> TestResult:
        """Run a single test with timeout and proper cleanup"""
        process = None
        try:
            start_time = time.time()

            # Use Popen for better process control
            popen_kwargs = {
                'stdout': subprocess.PIPE,
                'stderr': subprocess.PIPE,
                'text': True,
                'cwd': test_path.parent,
            }

            if sys.platform == 'win32':
                creationflags = 0
                if hasattr(subprocess, 'CREATE_NEW_PROCESS_GROUP'):
                    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
                popen_kwargs['creationflags'] = creationflags
            else:
                popen_kwargs['start_new_session'] = True

            process = subprocess.Popen([str(test_path)], **popen_kwargs)

            # Wait with timeout
            try:
                stdout, stderr = process.communicate(timeout=self.timeout)
                duration = time.time() - start_time

                success = (process.returncode == 0)
                return TestResult(
                    success=success,
                    duration=duration,
                    output=stdout,
                    error=stderr
                )

            except subprocess.TimeoutExpired as e:
                duration = self.timeout

                if self.preserve_on_timeout:
                    print(f"\n{Color.RED}Process stalled (PID {process.pid}). Preserving for debugging as requested.{Color.RESET}")
                    print(f"{Color.YELLOW}Attach a debugger to PID {process.pid} or terminate it manually when done.{Color.RESET}")
                    sys.exit(2)

                # Kill the process tree on timeout
                self._kill_process_tree(process.pid)

                # Get partial output from the exception object itself.
                # This avoids a second communicate() call which can hang.
                stdout = e.stdout if e.stdout else ""
                stderr = e.stderr if e.stderr else ""

                return TestResult(
                    success=False,
                    duration=duration,
                    output=stdout,
                    error=f"Test timed out after {self.timeout}s and was terminated.\n{stderr}"
                )

        except Exception as e:
            if process:
                self._kill_process_tree(process.pid)
            return TestResult(
                success=False,
                duration=0,
                error=f"Exception: {str(e)}"
            )

    def _kill_process_tree(self, pid: int):
        """Kill a process and all its children"""
        try:
            if sys.platform == 'win32':
                # On Windows, use taskkill to kill process tree.
                # Redirect output to DEVNULL to avoid hanging.
                subprocess.run(
                    ['taskkill', '/F', '/T', '/PID', str(pid)],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=5
                )
            else:
                # On Unix, kill process group
                import signal
                try:
                    pgid = os.getpgid(pid)
                except ProcessLookupError:
                    pgid = None

                if pgid is not None:
                    os.killpg(pgid, signal.SIGKILL)
                else:
                    os.kill(pid, signal.SIGKILL)
        except Exception as e:
            # Log but don't fail if cleanup fails
            print(f"\n{Color.YELLOW}Warning: Failed to kill process {pid}: {e}{Color.RESET}")
            pass

    def _kill_all_sintra_processes(self):
        """Kill all existing sintra processes to ensure clean start"""
        try:
            if sys.platform == 'win32':
                # Kill all sintra test processes
                test_names = [
                    'sintra_basic_pubsub_test.exe',
                    'sintra_ping_pong_test.exe',
                    'sintra_ping_pong_multi_test.exe',
                    'sintra_rpc_append_test.exe',
                    'sintra_recovery_test.exe',
                ]
                for name in test_names:
                    subprocess.run(
                        ['taskkill', '/F', '/IM', name],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        timeout=5
                    )
            else:
                # On Unix, use pkill. Redirect output to DEVNULL to avoid hanging.
                subprocess.run(
                    ['pkill', '-9', 'sintra'],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=5
                )
        except Exception:
            # Ignore errors - processes may not exist
            pass

    def run_test_multiple(self, test_path: Path, repetitions: int) -> Tuple[int, int, List[TestResult]]:
        """Run a test multiple times and collect results"""
        test_name = test_path.stem

        print(f"\n{Color.BOLD}{Color.BLUE}Running: {test_name}{Color.RESET}")
        print(f"  Repetitions: {repetitions}, Timeout: {self.timeout}s")
        print(f"  Progress: ", end='', flush=True)

        results = []
        passed = 0
        failed = 0

        for i in range(repetitions):
            result = self.run_test_once(test_path)
            results.append(result)

            if result.success:
                passed += 1
                print(f"{Color.GREEN}.{Color.RESET}", end='', flush=True)
            else:
                failed += 1
                print(f"{Color.RED}F{Color.RESET}", end='', flush=True)

            # Print newline every 50 tests for readability
            if (i + 1) % 50 == 0:
                print(f" [{i + 1}/{repetitions}]", end='', flush=True)
                if i + 1 < repetitions:
                    print("\n            ", end='', flush=True)

        print()  # Final newline

        return passed, failed, results

    def run_full_test_suite_once(self, tests: List[Path], repetitions: int):
        """Run all tests with given repetitions, return results per test"""
        test_results = {}
        total_runs = 0
        total_passed = 0

        for test_path in tests:
            test_name = test_path.stem
            test_passed = 0
            test_failed = 0
            successful_durations = []

            for _ in range(repetitions):
                result = self.run_test_once(test_path)
                total_runs += 1
                if result.success:
                    test_passed += 1
                    total_passed += 1
                    successful_durations.append(result.duration)
                else:
                    test_failed += 1

            avg_duration = sum(successful_durations) / len(successful_durations) if successful_durations else 0
            test_results[test_name] = (test_passed, test_failed, avg_duration)

        all_pass = total_runs == total_passed
        return all_pass, total_passed, total_runs, test_results

    def print_summary(self, test_path: Path, passed: int, failed: int, results: List[TestResult]):
        """Print summary statistics for a test"""
        test_name = test_path.stem
        total = passed + failed
        pass_rate = (passed / total * 100) if total > 0 else 0

        # Calculate duration statistics
        durations = [r.duration for r in results]
        avg_duration = sum(durations) / len(durations) if durations else 0
        min_duration = min(durations) if durations else 0
        max_duration = max(durations) if durations else 0

        if failed == 0:
            status = f"{Color.GREEN}PASS{Color.RESET}"
        else:
            status = f"{Color.RED}FAIL{Color.RESET}"

        print(f"  {Color.BOLD}Result: {status}{Color.RESET}")
        print(f"  Passed: {Color.GREEN}{passed}{Color.RESET} / Failed: {Color.RED}{failed}{Color.RESET} / Total: {total}")
        print(f"  Pass Rate: {pass_rate:.1f}%")
        print(f"  Duration: avg={format_duration(avg_duration)}, min={format_duration(min_duration)}, max={format_duration(max_duration)}")

        # Print details of failures if verbose or if there are failures
        if failed > 0 and (self.verbose or failed <= 5):
            print(f"\n  {Color.YELLOW}Failure Details:{Color.RESET}")
            failure_count = 0
            for i, result in enumerate(results):
                if not result.success:
                    failure_count += 1
                    print(f"    Run #{i+1}: {result.error[:100]}")
                    if self.verbose and result.output:
                        print(f"      stdout: {result.output[:200]}")
                    if self.verbose and result.error:
                        print(f"      stderr: {result.error[:200]}")

                    if failure_count >= 5 and not self.verbose:
                        remaining = failed - failure_count
                        if remaining > 0:
                            print(f"    ... and {remaining} more failures (use --verbose to see all)")
                        break

def main():
    parser = argparse.ArgumentParser(
        description='Run Sintra tests with timeout and repetition support',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument('--repetitions', type=int, default=100,
                        help='Number of times to run each test (default: 100)')
    parser.add_argument('--timeout', type=float, default=5.0,
                        help='Timeout per test run in seconds (default: 5)')
    parser.add_argument('--test', type=str, default=None,
                        help='Run only specific test (e.g., ping_pong)')
    parser.add_argument('--build-dir', type=str, default='../build-ninja2',
                        help='Path to build directory (default: ../build-ninja2)')
    parser.add_argument('--config', type=str, default='Debug',
                        choices=['Debug', 'Release'],
                        help='Build configuration (default: Debug)')
    parser.add_argument('--verbose', action='store_true',
                        help='Show detailed output for each test run')
    parser.add_argument('--preserve-stalled-processes', action='store_true',
                        help='Leave stalled test processes running for debugging instead of terminating them')
    parser.add_argument('--kill_stalled_processes', action='store_true', help=argparse.SUPPRESS)

    args = parser.parse_args()

    # Resolve build directory
    script_dir = Path(__file__).parent
    build_dir = (script_dir / args.build_dir).resolve()

    if not build_dir.exists():
        print(f"{Color.RED}Error: Build directory not found: {build_dir}{Color.RESET}")
        print(f"Please build the project first or specify correct --build-dir")
        return 1

    print(f"{Color.BOLD}Sintra Test Runner{Color.RESET}")
    print(f"Build directory: {build_dir}")
    print(f"Configuration: {args.config}")
    print(f"Repetitions per test: {args.repetitions}")
    print(f"Timeout per test: {args.timeout}s")
    print("=" * 70)

    if args.kill_stalled_processes:
        print(f"{Color.YELLOW}Warning: --kill_stalled_processes is deprecated; stalled tests are killed by default.{Color.RESET}")

    preserve_on_timeout = args.preserve_stalled_processes
    runner = TestRunner(build_dir, args.config, args.timeout, args.verbose, preserve_on_timeout)
    tests = runner.find_tests(args.test)

    if not tests:
        print(f"{Color.RED}No tests found to run{Color.RESET}")
        return 1

    print(f"Found {len(tests)} test(s) to run")

    start_time = time.time()

    # Adaptive soak test: run full suite with exponentially increasing batch sizes
    # Each batch runs the full suite, accumulating results
    accumulated_results = {test.stem: {'passed': 0, 'failed': 0, 'durations': []} for test in tests}

    batch_size = 1
    total_reps_so_far = 0
    max_reps_per_test = args.repetitions
    all_passed = True

    while total_reps_so_far < max_reps_per_test:
        # Calculate how many reps to run in this batch
        remaining = max_reps_per_test - total_reps_so_far
        current_batch = min(batch_size, remaining)

        all_pass, passed, total, test_results = runner.run_full_test_suite_once(tests, current_batch)
        total_reps_so_far += current_batch

        # Accumulate results
        for test_name, (test_passed, test_failed, avg_duration) in test_results.items():
            accumulated_results[test_name]['passed'] += test_passed
            accumulated_results[test_name]['failed'] += test_failed
            accumulated_results[test_name]['durations'].append(avg_duration)

        # Early stopping conditions
        if not all_pass:
            all_passed = False
            fail_rate = 100.0 - (passed / total * 100 if total > 0 else 0)
            if fail_rate >= 100 and total_reps_so_far >= 2:
                break
            elif fail_rate >= 30 and total_reps_so_far >= 4:
                break
            elif fail_rate >= 10 and total_reps_so_far >= 16:
                break

        # Double the batch size for next round
        batch_size *= 2

    # Print final results
    print("\n" + "=" * 80)
    print(f"{'Test':<40} {'Pass rate':<20} {'Avg runtime (s)':>15}")
    print("=" * 80)

    for test_name in sorted(accumulated_results.keys()):
        passed = accumulated_results[test_name]['passed']
        failed = accumulated_results[test_name]['failed']
        total = passed + failed
        pass_rate = (passed / total * 100) if total > 0 else 0

        durations = accumulated_results[test_name]['durations']
        avg_duration = sum(durations) / len(durations) if durations else 0

        pass_rate_str = f"{passed}/{total} ({pass_rate:6.2f}%)"
        print(f"{test_name:<40} {pass_rate_str:<20} {avg_duration:>15.2f}")

    print("=" * 80)

    total_duration = time.time() - start_time
    print(f"Total duration: {format_duration(total_duration)}")
    print()

    if all_passed:
        print(f"{Color.GREEN}PASSED{Color.RESET}")
        return 0
    else:
        print(f"{Color.RED}FAILED{Color.RESET}")
        return 1

if __name__ == '__main__':
    sys.exit(main())
