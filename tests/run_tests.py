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
    --include PATTERN               Include only tests matching glob-style pattern (can be repeated)
    --exclude PATTERN               Exclude tests matching glob-style pattern (can be repeated)
    --build-dir PATH                Path to build directory (default: ../build-ninja2)
    --config CONFIG                 Build configuration Debug/Release (default: Debug)
    --verbose                       Show detailed output for each test run
    --preserve-stalled-processes    Keep stalled processes running for debugging (default: terminate)
"""

import argparse
import fnmatch
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Tuple, Optional

class Color:
    """ANSI color codes for terminal output"""
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


# Tests that should receive additional repetition weight. The multiplier value
# is applied on top of the global ``--repetitions`` argument.
TEST_WEIGHT_OVERRIDES = {
    # ipc_rings release stress tests
    "ipc_rings_tests_release:stress:stress_attach_detach_readers": 200,
    "ipc_rings_tests_release:stress:stress_multi_reader_throughput": 100,

    # ipc_rings release unit tests
    "ipc_rings_tests_release:unit:test_directory_helpers": 500,
    "ipc_rings_tests_release:unit:test_get_ring_configurations_properties": 500,
    "ipc_rings_tests_release:unit:test_mod_helpers": 500,
    "ipc_rings_tests_release:unit:test_multiple_readers_see_same_data": 500,
    "ipc_rings_tests_release:unit:test_reader_eviction_does_not_underflow_octile_counter": 30,
    "ipc_rings_tests_release:unit:test_ring_write_read_single_reader": 500,
    "ipc_rings_tests_release:unit:test_slow_reader_eviction_restores_status": 500,
    "ipc_rings_tests_release:unit:test_snapshot_raii": 500,
    "ipc_rings_tests_release:unit:test_streaming_reader_status_restored_after_eviction": 500,
    "ipc_rings_tests_release:unit:test_wait_for_new_data": 500,

    # Other release tests
    "barrier_flush_test_release": 20,
    "barrier_stress_test_release": 10,
    "basic_pubsub_test_release": 30,
    "ping_pong_multi_test_release": 10,
    "ping_pong_test_release": 200,
    "processing_fence_test_release": 20,
    "recovery_test_release": 10,
    "rpc_append_test_release": 100,
    "spawn_detached_test_release": 30,
}


def _canonical_test_name(name: str) -> str:
    """Return the canonical identifier used for weight lookups."""

    canonical = name.strip()
    if canonical.startswith("sintra_"):
        canonical = canonical[len("sintra_"):]
    if canonical.endswith("_adaptive"):
        canonical = canonical[: -len("_adaptive")]
    return canonical


def _lookup_test_weight(name: str) -> int:
    """Return the repetition weight for the provided test invocation."""

    canonical = _canonical_test_name(name)
    return TEST_WEIGHT_OVERRIDES.get(canonical, 1)

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


@dataclass(frozen=True)
class TestInvocation:
    """Represents a single invocation of a test executable"""

    path: Path
    name: str
    args: Tuple[str, ...] = ()

    def command(self) -> List[str]:
        return [str(self.path), *self.args]

class TestRunner:
    """Manages test execution with timeout and repetition"""

    def __init__(self, build_dir: Path, config: str, timeout: float, verbose: bool,
                 preserve_on_timeout: bool = False):
        self.build_dir = build_dir
        self.config = config
        self.timeout = timeout
        self.verbose = verbose
        self.preserve_on_timeout = preserve_on_timeout
        self._ipc_rings_cache: Dict[Path, List[Tuple[str, str]]] = {}

        # Determine test directory - check both with and without config subdirectory
        test_dir_with_config = build_dir / 'tests' / config
        test_dir_simple = build_dir / 'tests'

        if test_dir_with_config.exists():
            self.test_dir = test_dir_with_config
        else:
            self.test_dir = test_dir_simple

        # Kill any existing sintra processes for a clean start
        self._kill_all_sintra_processes()

    def find_test_suites(
        self,
        test_name: Optional[str] = None,
        include_patterns: Optional[List[str]] = None,
        exclude_patterns: Optional[List[str]] = None,
    ) -> dict:
        """Find all test executables organized by configuration suite"""
        if not self.test_dir.exists():
            print(f"{Color.RED}Test directory not found: {self.test_dir}{Color.RESET}")
            return {}

        # 2 configurations: adaptive policy in release and debug builds
        configurations = [
            'release',
            'debug'
        ]

        # Discover available tests dynamically so the runner adapts to new or removed binaries
        discovered_tests: Dict[str, List[TestInvocation]] = {
            config: [] for config in configurations
        }

        try:
            directory_entries = sorted(self.test_dir.iterdir())
        except OSError as exc:
            print(f"{Color.RED}Failed to inspect test directory {self.test_dir}: {exc}{Color.RESET}")
            return {}

        for entry in directory_entries:
            if not (entry.is_file() or entry.is_symlink()):
                continue

            normalized_name = entry.name
            if sys.platform == 'win32' and normalized_name.lower().endswith('.exe'):
                normalized_name = normalized_name[:-4]

            for config in configurations:
                suffix = f"_{config}"
                if not normalized_name.endswith(suffix):
                    continue

                base_name = normalized_name[:-len(suffix)]

                if not self._matches_filters(
                    base_name,
                    normalized_name,
                    test_name,
                    include_patterns,
                    exclude_patterns,
                ):
                    break

                invocations = self._expand_test_invocations(entry, base_name, normalized_name)
                if invocations:
                    discovered_tests[config].extend(invocations)
                break

        test_suites = {}
        for config, tests in discovered_tests.items():
            if not tests:
                continue

            test_suites[config] = tests

        if not test_suites:
            if any([test_name, include_patterns, exclude_patterns]):
                filters = []
                if test_name:
                    filters.append(f"--test '{test_name}'")
                if include_patterns:
                    includes = ', '.join(include_patterns)
                    filters.append(f"--include {includes}")
                if exclude_patterns:
                    excludes = ', '.join(exclude_patterns)
                    filters.append(f"--exclude {excludes}")
                filter_text = '; '.join(filters)
                print(f"{Color.YELLOW}No tests found after applying filters: {filter_text}.{Color.RESET}")
            else:
                print(f"{Color.RED}No test binaries found in {self.test_dir}.{Color.RESET}")
            return {}

        return test_suites

    @staticmethod
    def _matches_filters(
        base_name: str,
        normalized_name: str,
        test_name: Optional[str],
        include_patterns: Optional[List[str]],
        exclude_patterns: Optional[List[str]],
    ) -> bool:
        """Determine if a test name should be included based on provided filters."""

        candidate_names = [base_name, normalized_name]

        if test_name and all(test_name not in name for name in candidate_names):
            return False

        if include_patterns:
            include_match = any(
                fnmatch.fnmatch(name, pattern)
                for pattern in include_patterns
                for name in candidate_names
            )
            if not include_match:
                return False

        if exclude_patterns:
            if any(
                fnmatch.fnmatch(name, pattern)
                for pattern in exclude_patterns
                for name in candidate_names
            ):
                return False

        return True

    def _expand_test_invocations(
        self,
        entry: Path,
        base_name: str,
        normalized_name: str,
    ) -> List[TestInvocation]:
        if base_name == 'sintra_ipc_rings_tests':
            return self._expand_ipc_rings_invocations(entry, normalized_name)

        return [TestInvocation(path=entry, name=normalized_name)]

    def _expand_ipc_rings_invocations(
        self,
        entry: Path,
        normalized_name: str,
    ) -> List[TestInvocation]:
        selectors = self._list_ipc_rings_tests(entry)
        if not selectors:
            # Fallback to running the executable as a whole if discovery fails
            return [TestInvocation(path=entry, name=normalized_name)]

        invocations: List[TestInvocation] = []
        for category, test_name in selectors:
            display_name = f"{normalized_name}:{category}:{test_name}"
            args = ('--run', f"{category}:{test_name}")
            invocations.append(TestInvocation(entry, display_name, args))
        return invocations

    def _list_ipc_rings_tests(self, entry: Path) -> List[Tuple[str, str]]:
        if entry in self._ipc_rings_cache:
            return self._ipc_rings_cache[entry]

        command = [str(entry), '--list-tests']
        try:
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                cwd=entry.parent,
                timeout=max(5.0, self.timeout / 2)
            )
        except subprocess.TimeoutExpired:
            print(
                f"{Color.YELLOW}Warning: Listing tests for {entry.name} timed out; running as a single test.{Color.RESET}"
            )
            self._ipc_rings_cache[entry] = []
            return []

        if result.returncode != 0:
            print(
                f"{Color.YELLOW}Warning: Failed to list tests for {entry.name} (exit {result.returncode}). "
                f"stderr: {result.stderr.strip()}{Color.RESET}"
            )
            self._ipc_rings_cache[entry] = []
            return []

        selectors: List[Tuple[str, str]] = []
        for raw_line in result.stdout.splitlines():
            line = raw_line.strip()
            if not line:
                continue
            if ':' not in line:
                print(
                    f"{Color.YELLOW}Warning: Ignoring malformed test descriptor '{line}' from {entry.name}.{Color.RESET}"
                )
                continue
            category, name = line.split(':', 1)
            category = category.strip()
            name = name.strip()
            if category not in {'unit', 'stress'}:
                print(
                    f"{Color.YELLOW}Warning: Unknown category '{category}' for test '{name}' in {entry.name}.{Color.RESET}"
                )
                continue
            selectors.append((category, name))

        self._ipc_rings_cache[entry] = selectors
        return selectors

    def run_test_once(self, invocation: TestInvocation) -> TestResult:
        """Run a single test with timeout and proper cleanup"""
        process = None
        try:
            start_time = time.time()

            # Use Popen for better process control
            popen_kwargs = {
                'stdout': subprocess.PIPE,
                'stderr': subprocess.PIPE,
                'text': True,
                'cwd': invocation.path.parent,
            }

            if sys.platform == 'win32':
                creationflags = 0
                if hasattr(subprocess, 'CREATE_NEW_PROCESS_GROUP'):
                    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
                popen_kwargs['creationflags'] = creationflags
            else:
                popen_kwargs['start_new_session'] = True

            process = subprocess.Popen(invocation.command(), **popen_kwargs)

            # Wait with timeout
            try:
                stdout, stderr = process.communicate(timeout=self.timeout)
                duration = time.time() - start_time

                success = (process.returncode == 0)
                error_msg = stderr

                if not success:
                    # Categorize failure type for better diagnostics
                    if process.returncode < 0 or process.returncode > 128:
                        # Unix signal (negative) or Windows crash code (large positive like 0xC0000005)
                        error_msg = f"CRASH: Process terminated abnormally (exit code {process.returncode})\n{stderr}"
                    elif duration < 0.1:
                        # Exited almost immediately - likely crash or early abort
                        error_msg = f"EARLY EXIT: Process exited with code {process.returncode} after {duration:.3f}s (possible crash)\n{stderr}"
                    else:
                        # Normal test failure
                        error_msg = f"TEST FAILED: Exit code {process.returncode} after {duration:.2f}s\n{stderr}"

                return TestResult(
                    success=success,
                    duration=duration,
                    output=stdout,
                    error=error_msg
                )

            except subprocess.TimeoutExpired as e:
                duration = self.timeout

                if self.preserve_on_timeout:
                    print(f"\n{Color.RED}TIMEOUT: Process exceeded timeout of {self.timeout}s (PID {process.pid}). Preserving for debugging as requested.{Color.RESET}")
                    print(f"{Color.YELLOW}Attach a debugger to PID {process.pid} or terminate it manually when done.{Color.RESET}")
                    sys.exit(2)

                # Try to get output immediately before killing
                stdout = ""
                stderr = ""
                try:
                    # Try to read from pipes before killing (non-blocking if possible)
                    import select
                    import os
                    if hasattr(select, 'select'):
                        # Unix: check if data is available
                        pass  # Will use communicate after kill
                except:
                    pass

                # Kill the process tree on timeout
                self._kill_process_tree(process.pid)

                # Try to get any buffered output after killing
                try:
                    stdout_data, stderr_data = process.communicate(timeout=1)
                    stdout = stdout_data if stdout_data else ""
                    stderr = stderr_data if stderr_data else ""
                except:
                    # Fall back to exception object
                    stdout = e.stdout if e.stdout else ""
                    stderr = e.stderr if e.stderr else ""

                # DEBUGGING: Print stderr immediately to terminal for ipc_rings_tests
                if 'ipc_rings' in invocation.path.name:
                    print(f"\n{Color.RED}=== TIMEOUT DEBUG OUTPUT (ipc_rings_tests) ==={Color.RESET}")
                    print(f"stdout length: {len(stdout)}")
                    print(f"stderr length: {len(stderr)}")
                    if stderr:
                        print(f"{Color.YELLOW}stderr content:{Color.RESET}")
                        print(stderr)
                    if stdout:
                        print(f"{Color.YELLOW}stdout content:{Color.RESET}")
                        print(stdout)
                    print(f"{Color.RED}=== END TIMEOUT DEBUG ==={Color.RESET}\n")

                return TestResult(
                    success=False,
                    duration=duration,
                    output=stdout,
                    error=f"TIMEOUT: Test exceeded {self.timeout}s and was terminated.\n{stderr}"
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

    def run_test_multiple(self, invocation: TestInvocation, repetitions: int) -> Tuple[int, int, List[TestResult]]:
        """Run a test multiple times and collect results"""
        test_name = invocation.name

        print(f"\n{Color.BOLD}{Color.BLUE}Running: {test_name}{Color.RESET}")
        print(f"  Repetitions: {repetitions}, Timeout: {self.timeout}s")
        print(f"  Progress: ", end='', flush=True)

        results = []
        passed = 0
        failed = 0

        for i in range(repetitions):
            result = self.run_test_once(invocation)
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

    def print_summary(self, invocation: TestInvocation, passed: int, failed: int, results: List[TestResult]):
        """Print summary statistics for a test"""
        test_name = invocation.name
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

                    # For ipc_rings_tests timeout, show full stderr for debugging
                    if 'ipc_rings' in test_name and 'TIMEOUT' in result.error:
                        print(f"\n  {Color.RED}FULL DEBUG OUTPUT (first timeout):{Color.RESET}")
                        if result.error:
                            print(result.error)
                        if result.output:
                            print(f"\n  stdout:\n{result.output}")
                        break  # Only show first timeout in detail

                    if failure_count >= 5 and not self.verbose:
                        remaining = failed - failure_count
                        if remaining > 0:
                            print(f"    ... and {remaining} more failures (use --verbose to see all)")
                        break


def _collect_patterns(raw_patterns: Optional[List[str]]) -> List[str]:
    """Normalize include/exclude arguments into a flat list of glob patterns."""

    if not raw_patterns:
        return []

    patterns: List[str] = []
    for raw in raw_patterns:
        if not raw:
            continue
        for item in raw.split(','):
            item = item.strip()
            if item:
                patterns.append(item)

    return patterns


def main():
    parser = argparse.ArgumentParser(
        description='Run Sintra tests with timeout and repetition support',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument('--repetitions', type=int, default=1000,
                        help='Number of times to run each test (default: 1000)')
    parser.add_argument('--timeout', type=float, default=5.0,
                        help='Timeout per test run in seconds (default: 5)')
    parser.add_argument('--test', type=str, default=None,
                        help='Run only tests whose names include the provided substring (e.g., ping_pong)')
    parser.add_argument('--include', action='append', default=None, metavar='PATTERN',
                        help='Include only tests whose names match the given glob-style pattern. '
                             'Can be repeated or include comma-separated patterns.')
    parser.add_argument('--exclude', action='append', default=None, metavar='PATTERN',
                        help='Exclude tests whose names match the given glob-style pattern. '
                             'Can be repeated or include comma-separated patterns.')
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
    if args.test:
        print(f"Substring filter (--test): {args.test}")
    include_patterns = _collect_patterns(args.include)
    exclude_patterns = _collect_patterns(args.exclude)
    if include_patterns:
        print(f"Include patterns: {', '.join(include_patterns)}")
    if exclude_patterns:
        print(f"Exclude patterns: {', '.join(exclude_patterns)}")
    print("=" * 70)

    if args.kill_stalled_processes:
        print(f"{Color.YELLOW}Warning: --kill_stalled_processes is deprecated; stalled tests are killed by default.{Color.RESET}")

    preserve_on_timeout = args.preserve_stalled_processes
    runner = TestRunner(build_dir, args.config, args.timeout, args.verbose, preserve_on_timeout)
    test_suites = runner.find_test_suites(
        test_name=args.test,
        include_patterns=include_patterns,
        exclude_patterns=exclude_patterns,
    )

    if not test_suites:
        print(f"{Color.RED}No test suites found to run{Color.RESET}")
        return 1

    total_configs = len(test_suites)
    print(f"Found {total_configs} configuration suite(s) to run")

    overall_start_time = time.time()
    overall_all_passed = True

    # Run each configuration suite independently
    for config_idx, (config_name, tests) in enumerate(test_suites.items(), 1):
        print(f"\n{'=' * 80}")
        print(f"{Color.BOLD}{Color.BLUE}Configuration {config_idx}/{total_configs}: {config_name}{Color.RESET}")
        print(f"  Tests in suite: {len(tests)}")
        print(f"  Repetitions: {args.repetitions}")
        print(f"{'=' * 80}")

        suite_start_time = time.time()

        # Adaptive soak test for this suite
        accumulated_results = {
            invocation.name: {'passed': 0, 'failed': 0, 'durations': [], 'failures': []}
            for invocation in tests
        }
        test_weights = {invocation.name: _lookup_test_weight(invocation.name) for invocation in tests}
        target_repetitions = {
            name: max(args.repetitions * weight, 0)
            for name, weight in test_weights.items()
        }
        remaining_repetitions = target_repetitions.copy()
        suite_all_passed = True
        batch_size = 1

        weighted_tests = [
            (_canonical_test_name(name), weight, target_repetitions[name])
            for name, weight in test_weights.items()
            if weight != 1
        ]
        if weighted_tests:
            print(f"  Weighted tests:")
            for display_name, weight, total in sorted(weighted_tests):
                print(f"    {display_name}: x{weight} -> {total} repetition(s)")

        while True:
            remaining_counts = [count for count in remaining_repetitions.values() if count > 0]
            if not remaining_counts:
                break

            max_remaining = max(remaining_counts)
            reps_in_this_round = min(batch_size, max_remaining)
            print(f"\n{Color.BLUE}--- Round: {reps_in_this_round} repetition(s) ---{Color.RESET}")

            for i in range(reps_in_this_round):
                print(f"  Rep {i + 1}/{reps_in_this_round}: ", end="", flush=True)
                for invocation in tests:
                    test_name = invocation.name
                    if remaining_repetitions[test_name] <= 0:
                        continue

                    result = runner.run_test_once(invocation)

                    accumulated_results[test_name]['durations'].append(result.duration)

                    result_bucket = accumulated_results[test_name]

                    if result.success:
                        result_bucket['passed'] += 1
                        print(f"{Color.GREEN}.{Color.RESET}", end="", flush=True)
                    else:
                        result_bucket['failed'] += 1
                        suite_all_passed = False
                        overall_all_passed = False

                        run_index = result_bucket['passed'] + result_bucket['failed']
                        error_message = (result.error or '').strip()
                        if error_message:
                            first_line = error_message.splitlines()[0]
                        else:
                            first_line = 'No error output captured'

                        result_bucket['failures'].append({
                            'run': run_index,
                            'message': first_line,
                        })

                        print(f"{Color.RED}F{Color.RESET}", end="", flush=True)

                    remaining_repetitions[test_name] -= 1

                print()
                if not suite_all_passed:
                    break

            if not suite_all_passed:
                break

            batch_size = min(batch_size * 2, max_remaining)

        # Print suite results
        suite_duration = time.time() - suite_start_time
        def format_test_name(test_name):
            """Format the raw test name for display in the summary table."""

            formatted = test_name
            if formatted.startswith("sintra_"):
                formatted = formatted[len("sintra_"):]

            if formatted.startswith("ipc_rings_tests_"):
                formatted = formatted.replace("_release_adaptive", "_release", 1)
                formatted = formatted.replace("_debug_adaptive", "_debug", 1)
                if formatted.endswith("_release"):
                    formatted = formatted[:-len("_release")] + " (release)"
                elif formatted.endswith("_debug"):
                    formatted = formatted[:-len("_debug")] + " (debug)"

            return formatted

        def sort_key(test_name):
            formatted = format_test_name(test_name)
            if formatted.startswith("dummy_test"):
                group = 0
            elif formatted.startswith("ipc_rings_tests"):
                group = 1
            else:
                group = 2
            return group, formatted

        print(f"\n{Color.BOLD}Results for {config_name}:{Color.RESET}")

        ordered_test_names = sorted(accumulated_results.keys(), key=sort_key)
        formatted_names = [format_test_name(name) for name in ordered_test_names]

        test_col_width = max([len(name) for name in formatted_names] + [4]) + 2
        passrate_col_width = 20
        avg_runtime_col_width = 17

        header_fmt = (
            f"{{:<{test_col_width}}}"
            f" {{:>{passrate_col_width}}}"
            f" {{:>{avg_runtime_col_width}}}"
        )
        row_fmt = header_fmt
        table_width = test_col_width + passrate_col_width + avg_runtime_col_width + 2

        print("=" * table_width)
        print(header_fmt.format('Test', 'Pass rate', 'Avg runtime (s)'))
        print("=" * table_width)

        for test_name, display_name in zip(ordered_test_names, formatted_names):
            passed = accumulated_results[test_name]['passed']
            failed = accumulated_results[test_name]['failed']
            total = passed + failed
            pass_rate = (passed / total * 100) if total > 0 else 0

            durations = accumulated_results[test_name]['durations']
            avg_duration = sum(durations) / len(durations) if durations else 0

            pass_rate_str = f"{passed}/{total} ({pass_rate:6.2f}%)"
            avg_duration_str = f"{avg_duration:.2f}"
            print(row_fmt.format(display_name, pass_rate_str, avg_duration_str))

        print("=" * table_width)
        print(f"Suite duration: {format_duration(suite_duration)}")

        if suite_all_passed:
            print(f"Suite result: {Color.GREEN}PASSED{Color.RESET}")
        else:
            print(f"Suite result: {Color.RED}FAILED{Color.RESET}")
            failing_tests = {
                name: data['failures']
                for name, data in accumulated_results.items()
                if data['failures']
            }

            if failing_tests:
                print(f"\n{Color.YELLOW}Failure summary:{Color.RESET}")
                for test_name, failures in failing_tests.items():
                    print(f"  {test_name}:")
                    for failure in failures[:5]:
                        print(f"    Run #{failure['run']}: {failure['message']}")
                    if len(failures) > 5:
                        remaining = len(failures) - 5
                        print(f"    ... and {remaining} more failure(s)")
            print(f"\n{Color.RED}Stopping - suite {config_name} failed{Color.RESET}")
            break

    # Final summary
    total_duration = time.time() - overall_start_time
    print(f"\n{'=' * 80}")
    print(f"{Color.BOLD}OVERALL SUMMARY{Color.RESET}")
    print(f"Total duration: {format_duration(total_duration)}")

    if overall_all_passed:
        print(f"Overall result: {Color.GREEN}ALL SUITES PASSED{Color.RESET}")
        return 0
    else:
        print(f"Overall result: {Color.RED}FAILED{Color.RESET}")
        return 1

if __name__ == '__main__':
    sys.exit(main())
