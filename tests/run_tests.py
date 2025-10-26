#!/usr/bin/env python3
"""
Sintra Test Runner with Timeout and Repetition Support

This script runs Sintra tests multiple times with proper timeout handling
to detect non-deterministic failures caused by OS scheduling issues.

Usage:
    python run_tests.py [options]

Options:
    --repetitions N                 Number of times to run each test (default: 1)
    --timeout SECONDS               Timeout per test run in seconds (default: 5)
    --test NAME                     Run only specific test (e.g., sintra_ping_pong_test)
    --include PATTERN               Include only tests matching glob-style pattern (can be repeated)
    --exclude PATTERN               Exclude tests matching glob-style pattern (can be repeated)
    --build-dir PATH                Path to build directory (default: ../build-ninja2)
    --config CONFIG                 Build configuration Debug (default: Debug)
    --verbose                       Show detailed output for each test run
    --preserve-stalled-processes    Keep stalled processes running for debugging (default: terminate)
"""

import argparse
import fnmatch
import os
import shlex
import shutil
import subprocess
import sys
import threading
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

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
        self._debugger_cache: Dict[str, Tuple[Optional[str], str]] = {}

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

        # Only run the debug configuration for the crash reproduction scenario.
        configurations = ['debug']

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
            start_monotonic = time.monotonic()

            # Use Popen for better process control
            popen_kwargs = {
                'stdout': subprocess.PIPE,
                'stderr': subprocess.PIPE,
                'text': True,
                'bufsize': 1,
                'cwd': invocation.path.parent,
            }

            if sys.platform == 'win32':
                creationflags = 0
                if hasattr(subprocess, 'CREATE_NEW_PROCESS_GROUP'):
                    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
                popen_kwargs['creationflags'] = creationflags
            else:
                popen_kwargs['start_new_session'] = True

            stdout_lines: List[str] = []
            stderr_lines: List[str] = []
            live_stack_traces = ""
            live_stack_error = ""
            postmortem_stack_traces = ""
            postmortem_stack_error = ""
            failure_event = threading.Event()
            capture_lock = threading.Lock()
            capture_pause_total = 0.0
            capture_active_start: Optional[float] = None
            threads: List[threading.Thread] = []

            process = subprocess.Popen(invocation.command(), **popen_kwargs)

            def attempt_live_capture(trigger_line: str) -> None:
                nonlocal live_stack_traces, live_stack_error, capture_pause_total, capture_active_start
                if not trigger_line:
                    return
                if not self._line_indicates_failure(trigger_line):
                    return
                capture_started = time.monotonic()
                with capture_lock:
                    if failure_event.is_set():
                        return
                    failure_event.set()
                    capture_active_start = capture_started

                traces = ""
                error = ""
                try:
                    traces, error = self._capture_process_stacks(process.pid)
                finally:
                    capture_finished = time.monotonic()
                    with capture_lock:
                        if capture_active_start is not None:
                            capture_pause_total += capture_finished - capture_active_start
                        capture_active_start = None

                if traces:
                    with capture_lock:
                        if live_stack_traces:
                            live_stack_traces = f"{live_stack_traces}\n\n{traces}"
                        else:
                            live_stack_traces = traces
                        live_stack_error = ""
                elif error and not live_stack_traces:
                    with capture_lock:
                        live_stack_error = error

            def monitor_stream(stream, buffer: List[str]) -> None:
                try:
                    for line in iter(stream.readline, ''):
                        buffer.append(line)
                        attempt_live_capture(line)
                finally:
                    try:
                        stream.close()
                    except Exception:
                        pass

            if process.stdout:
                stdout_thread = threading.Thread(
                    target=monitor_stream,
                    args=(process.stdout, stdout_lines),
                    daemon=True,
                )
                stdout_thread.start()
                threads.append(stdout_thread)

            if process.stderr:
                stderr_thread = threading.Thread(
                    target=monitor_stream,
                    args=(process.stderr, stderr_lines),
                    daemon=True,
                )
                stderr_thread.start()
                threads.append(stderr_thread)

            # Wait with timeout, extending the deadline for live stack captures
            try:
                poll_interval = 0.1
                while True:
                    with capture_lock:
                        pause_total = capture_pause_total
                        active_start = capture_active_start

                    now_monotonic = time.monotonic()
                    active_extra = 0.0
                    if active_start is not None:
                        active_extra = now_monotonic - active_start

                    adjusted_deadline = (
                        start_monotonic + self.timeout + pause_total + active_extra
                    )

                    if process.poll() is not None:
                        break

                    remaining = adjusted_deadline - now_monotonic
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(process.args, self.timeout)

                    time.sleep(min(poll_interval, remaining))

                process.wait()
                duration = time.time() - start_time

                for thread in threads:
                    thread.join()

                stdout = ''.join(stdout_lines)
                stderr = ''.join(stderr_lines)

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

                    if not live_stack_traces and not live_stack_error:
                        traces, error = self._capture_process_stacks(process.pid)
                        if traces:
                            live_stack_traces = traces
                        elif error:
                            live_stack_error = error

                    if self._is_crash_exit(process.returncode):
                        traces, error = self._capture_process_stacks(process.pid)
                        if traces:
                            if live_stack_traces:
                                live_stack_traces = f"{live_stack_traces}\n\n{traces}"
                            else:
                                live_stack_traces = traces
                            live_stack_error = ""
                        elif error and not live_stack_traces:
                            live_stack_error = error

                        if sys.platform == 'win32':
                            (
                                postmortem_stack_traces,
                                postmortem_stack_error,
                            ) = self._capture_windows_crash_dump(invocation, start_time, process.pid)
                        else:
                            (
                                postmortem_stack_traces,
                                postmortem_stack_error,
                            ) = self._capture_core_dump_stack(invocation, start_time, process.pid)

                if live_stack_traces:
                    error_msg = f"{error_msg}\n\n=== Captured stack traces ===\n{live_stack_traces}"
                elif live_stack_error:
                    error_msg = f"{error_msg}\n\n[Stack capture unavailable: {live_stack_error}]"

                if postmortem_stack_traces:
                    error_msg = f"{error_msg}\n\n=== Post-mortem stack trace ===\n{postmortem_stack_traces}"
                elif postmortem_stack_error:
                    error_msg = f"{error_msg}\n\n[Post-mortem stack capture unavailable: {postmortem_stack_error}]"

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

                stack_traces = live_stack_traces
                stack_error = live_stack_error
                if process:
                    extra_traces, extra_error = self._capture_process_stacks(process.pid)
                    if extra_traces:
                        if stack_traces:
                            stack_traces = f"{stack_traces}\n\n{extra_traces}"
                        else:
                            stack_traces = extra_traces
                        stack_error = ""
                    elif extra_error and not stack_traces:
                        stack_error = extra_error

                # Kill the process tree on timeout
                self._kill_process_tree(process.pid)

                try:
                    process.wait(timeout=1)
                except Exception:
                    pass

                for thread in threads:
                    thread.join()

                stdout = ''.join(stdout_lines)
                stderr = ''.join(stderr_lines)

                if stack_traces:
                    stderr = f"{stderr}\n\n=== Captured stack traces ===\n{stack_traces}"
                elif stack_error:
                    stderr = f"{stderr}\n\n[Stack capture unavailable: {stack_error}]"

                return TestResult(
                    success=False,
                    duration=duration,
                    output=stdout,
                    error=f"TIMEOUT: Test exceeded {self.timeout}s and was terminated.\n{stderr}"
                )

        except Exception as e:
            if process:
                self._kill_process_tree(process.pid)
            for thread in threads:
                try:
                    thread.join(timeout=1)
                except Exception:
                    pass
            stdout = ''.join(stdout_lines) if stdout_lines else ""
            stderr = ''.join(stderr_lines) if stderr_lines else ""
            error_msg = f"Exception: {str(e)}"
            if stderr:
                error_msg = f"{error_msg}\n{stderr}"
            return TestResult(
                success=False,
                duration=0,
                output=stdout,
                error=error_msg
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

    def _line_indicates_failure(self, line: str) -> bool:
        """Heuristically determine whether a log line signals a test failure."""

        lowered = line.strip().lower()
        if not lowered:
            return False

        failure_markers = (
            '[fail',
            '[  failed',
            ' assertion failed',
            'assertion failed:',
            'assertion_error',
            'fatal error',
            'runtime error',
            ' panic:',
            'test failed',
            'unhandled exception',
            'addresssanitizer',
            'ubsan:',
            'terminate called',
            'segmentation fault',
        )

        return any(marker in lowered for marker in failure_markers)

    def _is_crash_exit(self, returncode: int) -> bool:
        """Return True if the exit code represents an abnormal termination."""

        if returncode == 0:
            return False

        if sys.platform == 'win32':
            # Windows crash codes are typically large unsigned values such as 0xC0000005.
            return returncode < 0 or returncode >= 0xC0000000

        # POSIX: negative return codes indicate termination by signal.
        # Codes > 128 are also commonly used to report signal-based exits.
        return returncode < 0 or returncode > 128

    def _capture_process_stacks(self, pid: int) -> Tuple[str, str]:
        """Attempt to capture stack traces for the test process and all of its helpers."""

        if sys.platform == 'win32':
            return self._capture_process_stacks_windows(pid)

        debugger_name, debugger_command, debugger_error = self._resolve_unix_debugger()
        if not debugger_command:
            return "", debugger_error

        import signal
        try:
            pgid = os.getpgid(pid)
        except ProcessLookupError:
            return "", "process exited before stack capture"

        target_pids = set()
        target_pids.add(pid)
        target_pids.update(self._collect_process_group_pids(pgid))
        target_pids.update(self._collect_descendant_pids(pid))

        if not target_pids:
            target_pids = {pid}

        paused_pids = []
        stack_outputs = []
        capture_errors = []

        # Pause each discovered process individually so we can capture consistent stacks,
        # even if helpers spawned new process groups.
        try:
            import signal
        except ImportError:
            signal = None

        if signal is not None:
            for target_pid in sorted(target_pids):
                if target_pid == os.getpid():
                    continue
                try:
                    os.kill(target_pid, signal.SIGSTOP)
                    paused_pids.append(target_pid)
                except (ProcessLookupError, PermissionError):
                    continue

        for target_pid in sorted(target_pids):
            if target_pid == os.getpid():
                continue

            try:
                result = subprocess.run(
                    self._build_unix_live_debugger_command(
                        debugger_name,
                        debugger_command,
                        target_pid,
                    ),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=30,
                )
            except subprocess.SubprocessError as exc:
                capture_errors.append(
                    f"PID {target_pid}: {debugger_name} failed ({exc})"
                )
                continue

            output = result.stdout.strip()
            if not output and result.stderr:
                output = result.stderr.strip()

            if result.returncode != 0:
                capture_errors.append(
                    f"PID {target_pid}: {debugger_name} exited with code {result.returncode}: {result.stderr.strip()}"
                )
                continue

            if output:
                stack_outputs.append(f"PID {target_pid}\n{output}")

        # Allow processes to continue so gdb can detach cleanly before killing
        if signal is not None:
            for target_pid in paused_pids:
                try:
                    os.kill(target_pid, signal.SIGCONT)
                except (ProcessLookupError, PermissionError):
                    continue

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no stack data captured"

    def _capture_core_dump_stack(
        self,
        invocation: TestInvocation,
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        """Attempt to capture stack traces from a recently generated core dump."""

        if sys.platform == 'win32':
            return "", "core dump stack capture not supported on Windows"

        debugger_name, debugger_command, debugger_error = self._resolve_unix_debugger()
        if not debugger_command:
            return "", debugger_error

        candidate_dirs = [invocation.path.parent, Path.cwd()]

        if sys.platform == 'darwin':
            darwin_core_dirs = [Path('/cores')]
            darwin_core_dirs.append(Path.home() / 'Library' / 'Logs' / 'DiagnosticReports')

            for directory in darwin_core_dirs:
                if directory not in candidate_dirs:
                    candidate_dirs.append(directory)
        candidate_cores: List[Tuple[float, Path]] = []

        for directory in candidate_dirs:
            try:
                entries = list(directory.iterdir())
            except OSError:
                continue

            for entry in entries:
                if not entry.is_file():
                    continue

                name = entry.name
                if not (
                    name == 'core'
                    or name.startswith('core.')
                    or name.startswith('core-')
                    or name.endswith('.core')
                ):
                    continue

                try:
                    stat_info = entry.stat()
                except OSError:
                    continue

                # Only consider cores generated after the process started.
                if stat_info.st_mtime + 0.001 < start_time:
                    continue

                candidate_cores.append((stat_info.st_mtime, entry))

        if not candidate_cores:
            return "", "no recent core dump found"

        # Prefer cores whose filename contains the PID of the crashed process.
        pid_str = str(pid)
        prioritized = [item for item in candidate_cores if pid_str in item[1].name]
        if prioritized:
            candidate_cores = prioritized

        # Sort newest first to analyse the most relevant dumps first.
        candidate_cores.sort(key=lambda item: item[0], reverse=True)

        stack_outputs: List[str] = []
        capture_errors: List[str] = []

        for _, core_path in candidate_cores:
            try:
                result = subprocess.run(
                    self._build_unix_core_debugger_command(
                        debugger_name,
                        debugger_command,
                        invocation,
                        core_path,
                    ),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=60,
                )
            except subprocess.SubprocessError as exc:
                capture_errors.append(
                    f"{core_path.name}: {debugger_name} failed ({exc})"
                )
                continue

            output = result.stdout.strip()
            if not output and result.stderr:
                output = result.stderr.strip()

            if result.returncode != 0:
                capture_errors.append(
                    f"{core_path.name}: {debugger_name} exited with code {result.returncode}: {result.stderr.strip()}"
                )
                continue

            if output:
                stack_outputs.append(f"core file: {core_path}\n{output}")
                break

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no usable core dump found"

    def _resolve_unix_debugger(self) -> Tuple[Optional[str], Optional[List[str]], str]:
        """Locate gdb or lldb on Unix-like platforms."""

        if sys.platform == 'win32':
            return None, None, 'debugger not available on this platform'

        gdb_path = shutil.which('gdb')
        if gdb_path:
            return 'gdb', [gdb_path], ''

        lldb_path = shutil.which('lldb')
        if lldb_path:
            return 'lldb', [lldb_path], ''

        xcrun_path = shutil.which('xcrun')
        if xcrun_path:
            return 'lldb', [xcrun_path, 'lldb'], ''

        return None, None, 'gdb or lldb not available (install gdb or the Xcode Command Line Tools)'

    def _build_unix_live_debugger_command(
        self,
        debugger_name: str,
        debugger_command: List[str],
        pid: int,
    ) -> List[str]:
        """Construct the debugger command to attach to a live process."""

        if debugger_name == 'gdb':
            return [
                *debugger_command,
                '--batch',
                '-p', str(pid),
                '-ex', 'set pagination off',
                '-ex', 'thread apply all bt',
                '-ex', 'detach',
            ]

        # Fallback to lldb
        return [
            *debugger_command,
            '--batch',
            '-p', str(pid),
            '-o', 'thread backtrace all',
            '-o', 'detach',
            '-o', 'quit',
        ]

    def _build_unix_core_debugger_command(
        self,
        debugger_name: str,
        debugger_command: List[str],
        invocation: TestInvocation,
        core_path: Path,
    ) -> List[str]:
        """Construct the debugger command to inspect a generated core file."""

        if debugger_name == 'gdb':
            return [
                *debugger_command,
                '--batch',
                '-ex', 'set pagination off',
                '-ex', 'thread apply all bt',
                str(invocation.path),
                str(core_path),
            ]

        # Fallback to lldb
        quoted_executable = shlex.quote(str(invocation.path))
        quoted_core = shlex.quote(str(core_path))

        return [
            *debugger_command,
            '--batch',
            '-o', f'target create {quoted_executable}',
            '-o', f'process attach -c {quoted_core}',
            '-o', 'thread backtrace all',
            '-o', 'quit',
        ]

    def _resolve_windows_debugger(self) -> Tuple[Optional[str], Optional[str], str]:
        """Locate or install a Windows debugger suitable for stack capture."""

        if sys.platform != 'win32':
            return None, None, 'debugger not available on this platform'

        debugger_candidates = ['windbg', 'cdb', 'ntsd']
        errors: List[str] = []

        for candidate in debugger_candidates:
            cache_key = f'windows:{candidate}'
            if cache_key in self._debugger_cache:
                path, error = self._debugger_cache[cache_key]
            else:
                path = shutil.which(candidate)
                error = '' if path else f'{candidate} not found in PATH'
                self._debugger_cache[cache_key] = (path, error)
            if path:
                return candidate, path, ''
            if error:
                errors.append(error)

        winget_path = shutil.which('winget')
        if not winget_path:
            combined = '; '.join(errors) if errors else 'no Windows debugger available'
            return None, None, f'{combined}; winget not available to install WinDbg'

        try:
            result = subprocess.run(
                [winget_path, 'install', '--id', 'Microsoft.WinDbg', '-e'],
                check=False,
                capture_output=True,
                text=True,
            )
        except Exception as exc:
            combined = '; '.join(errors) if errors else ''
            prefix = f'{combined}; ' if combined else ''
            return None, None, f'{prefix}failed to run winget: {exc}'

        if result.returncode != 0:
            details = result.stderr.strip() or result.stdout.strip()
            combined = '; '.join(errors) if errors else ''
            prefix = f'{combined}; ' if combined else ''
            if details:
                return None, None, f'{prefix}winget install failed ({result.returncode}): {details}'
            return None, None, f'{prefix}winget install failed ({result.returncode})'

        for candidate in debugger_candidates:
            path = shutil.which(candidate)
            if path:
                cache_key = f'windows:{candidate}'
                self._debugger_cache[cache_key] = (path, '')
                return candidate, path, ''

        combined = '; '.join(errors) if errors else ''
        prefix = f'{combined}; ' if combined else ''
        return None, None, f'{prefix}WinDbg installation succeeded but debugger still not found'

    def _capture_windows_crash_dump(
        self,
        invocation: TestInvocation,
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        """Attempt to capture stack traces from recent Windows crash dump files."""

        debugger_name, debugger_path, debugger_error = self._resolve_windows_debugger()
        if not debugger_path:
            return "", debugger_error

        candidate_dirs = [invocation.path.parent, Path.cwd()]

        local_app_data = os.environ.get('LOCALAPPDATA')
        if local_app_data:
            candidate_dirs.append(Path(local_app_data) / 'CrashDumps')
        else:
            candidate_dirs.append(Path.home() / 'AppData' / 'Local' / 'CrashDumps')

        exe_name_lower = invocation.path.name.lower()
        exe_stem_lower = invocation.path.stem.lower()
        pid_str = str(pid)

        candidate_dumps: List[Tuple[float, Path]] = []

        for directory in candidate_dirs:
            try:
                if not directory or not directory.exists():
                    continue
                entries = list(directory.iterdir())
            except OSError:
                continue

            for entry in entries:
                if not entry.is_file():
                    continue

                name_lower = entry.name.lower()
                if not name_lower.endswith('.dmp'):
                    continue

                if exe_name_lower not in name_lower and exe_stem_lower not in name_lower:
                    continue

                try:
                    stat_info = entry.stat()
                except OSError:
                    continue

                if stat_info.st_mtime + 0.001 < start_time:
                    continue

                candidate_dumps.append((stat_info.st_mtime, entry))

        if not candidate_dumps:
            return "", "no recent crash dump found"

        prioritized = [item for item in candidate_dumps if pid_str in item[1].name]
        if prioritized:
            candidate_dumps = prioritized

        candidate_dumps.sort(key=lambda item: item[0], reverse=True)

        stack_outputs: List[str] = []
        capture_errors: List[str] = []

        for _, dump_path in candidate_dumps:
            try:
                command = [debugger_path]
                if debugger_name == 'windbg':
                    command.append('-Q')
                command.extend(['-z', str(dump_path), '-c', '.symfix; .reload; ~* k; qd'])

                result = subprocess.run(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=120,
                )
            except subprocess.SubprocessError as exc:
                capture_errors.append(f"{dump_path.name}: {debugger_name} failed ({exc})")
                continue

            output = result.stdout.strip()
            if not output and result.stderr:
                output = result.stderr.strip()

            if result.returncode != 0:
                capture_errors.append(
                    f"{dump_path.name}: {debugger_name} exited with code {result.returncode}: {result.stderr.strip()}"
                )
                continue

            if output:
                stack_outputs.append(f"dump file: {dump_path}\n{output}")
                break

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no usable crash dump found"

    def _capture_process_stacks_windows(self, pid: int) -> Tuple[str, str]:
        """Capture stack traces for a process tree on Windows using an installed debugger."""

        debugger_name, debugger_path, debugger_error = self._resolve_windows_debugger()
        if not debugger_path:
            return "", debugger_error

        target_pids = self._collect_windows_process_tree_pids(pid)
        target_pids.append(pid)

        stack_outputs: List[str] = []
        capture_errors: List[str] = []

        for target_pid in sorted(set(target_pids)):
            try:
                command = [debugger_path]
                if debugger_name == 'windbg':
                    command.append('-Q')
                command.extend(['-p', str(target_pid), '-c', '.symfix; .reload; ~* k; qd'])

                result = subprocess.run(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=60,
                )
            except FileNotFoundError:
                fallback_error = debugger_error or f"{debugger_name} not available"
                return "", fallback_error
            except subprocess.SubprocessError as exc:
                capture_errors.append(f"PID {target_pid}: {debugger_name} failed ({exc})")
                continue

            output = result.stdout.strip()
            if not output and result.stderr:
                output = result.stderr.strip()

            if result.returncode != 0:
                capture_errors.append(
                    f"PID {target_pid}: {debugger_name} exited with code {result.returncode}: {result.stderr.strip()}"
                )
                continue

            if output:
                stack_outputs.append(f"PID {target_pid}\n{output}")

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no stack data captured"

    def _collect_windows_process_tree_pids(self, pid: int) -> List[int]:
        """Return all descendant process IDs for the provided Windows process."""

        powershell_path = shutil.which('powershell')
        if not powershell_path:
            return []

        script = (
            "function Get-ChildPids($Pid){"
            "  $children = Get-CimInstance Win32_Process -Filter \"ParentProcessId=$Pid\";"
            "  foreach($child in $children){"
            "    $child.ProcessId;"
            "    Get-ChildPids $child.ProcessId"
            "  }"
            "}"
            f"; Get-ChildPids {pid}"
        )

        try:
            result = subprocess.run(
                [
                    powershell_path,
                    '-NoProfile',
                    '-Command',
                    script,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=30,
            )
        except (subprocess.SubprocessError, OSError):
            return []

        if result.returncode != 0:
            return []

        descendants: List[int] = []
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                descendants.append(int(line))
            except ValueError:
                continue

        return descendants

    def _collect_process_group_pids(self, pgid: int) -> List[int]:
        """Return all process IDs belonging to the provided process group."""

        pids: List[int] = []

        if sys.platform == 'win32':
            return pids

        proc_path = Path('/proc')
        entries: Optional[List[Path]] = None
        if proc_path.exists():
            try:
                entries = list(proc_path.iterdir())
            except Exception:
                entries = None

        if entries is None:
            return self._collect_process_group_pids_via_ps(pgid)

        for entry in entries:
            if not entry.is_dir():
                continue
            name = entry.name
            if not name.isdigit():
                continue

            try:
                candidate_pid = int(name)
            except ValueError:
                continue

            try:
                candidate_pgid = os.getpgid(candidate_pid)
            except (ProcessLookupError, PermissionError):
                continue

            if candidate_pgid == pgid:
                pids.append(candidate_pid)

        return pids

    def _collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Return all descendant process IDs for the provided root PID on Unix."""

        descendants: List[int] = []

        if sys.platform == 'win32':
            return descendants

        proc_path = Path('/proc')
        entries: Optional[List[Path]] = None
        if proc_path.exists():
            try:
                entries = list(proc_path.iterdir())
            except Exception:
                entries = None

        if entries is None:
            return self._collect_descendant_pids_via_ps(root_pid)

        parent_to_children: Dict[int, List[int]] = {}

        for entry in entries:
            if not entry.is_dir():
                continue

            name = entry.name
            if not name.isdigit():
                continue

            try:
                pid = int(name)
            except ValueError:
                continue

            stat_path = entry / 'stat'
            try:
                stat_content = stat_path.read_text()
            except (OSError, UnicodeDecodeError):
                continue

            close_paren = stat_content.find(')')
            if close_paren == -1 or close_paren + 2 >= len(stat_content):
                continue

            remainder = stat_content[close_paren + 2 :].split()
            if len(remainder) < 2:
                continue

            try:
                parent_pid = int(remainder[1])
            except ValueError:
                continue

            parent_to_children.setdefault(parent_pid, []).append(pid)

        visited: Set[int] = set()
        stack: List[int] = parent_to_children.get(root_pid, [])[:]

        while stack:
            current = stack.pop()
            if current in visited or current == root_pid:
                continue
            visited.add(current)
            descendants.append(current)
            stack.extend(parent_to_children.get(current, []))

        return descendants

    def _snapshot_process_table_via_ps(self) -> List[Tuple[int, int, int]]:
        """Return (pid, ppid, pgid) tuples using the portable ps command."""

        if sys.platform == 'win32':
            return []

        ps_executable = shutil.which('ps')
        if not ps_executable:
            return []

        try:
            result = subprocess.run(
                [ps_executable, '-eo', 'pid=,ppid=,pgid='],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
                check=True,
            )
        except subprocess.SubprocessError:
            return []

        snapshot: List[Tuple[int, int, int]] = []
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 3:
                continue
            try:
                pid_value = int(parts[0])
                ppid_value = int(parts[1])
                pgid_value = int(parts[2])
            except ValueError:
                continue
            snapshot.append((pid_value, ppid_value, pgid_value))

        return snapshot

    def _collect_process_group_pids_via_ps(self, pgid: int) -> List[int]:
        """Collect process IDs that belong to the provided PGID using ps."""

        snapshot = self._snapshot_process_table_via_ps()
        if not snapshot:
            return []

        return [pid for pid, _, process_group in snapshot if process_group == pgid]

    def _collect_descendant_pids_via_ps(self, root_pid: int) -> List[int]:
        """Collect descendant process IDs for the provided root PID using ps."""

        snapshot = self._snapshot_process_table_via_ps()
        if not snapshot:
            return []

        parent_to_children: Dict[int, List[int]] = defaultdict(list)
        for pid, ppid, _ in snapshot:
            parent_to_children[ppid].append(pid)

        descendants: List[int] = []
        stack: List[int] = parent_to_children.get(root_pid, [])[:]
        while stack:
            current = stack.pop()
            if current in descendants or current == root_pid:
                continue
            descendants.append(current)
            stack.extend(parent_to_children.get(current, []))

        return descendants

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

                    full_error_needed = (
                        self.verbose
                        or '=== Captured stack traces ===' in result.error
                        or '=== Post-mortem stack trace ===' in result.error
                        or '[Stack capture unavailable' in result.error
                    )

                    if full_error_needed:
                        error_lines = result.error.splitlines() or [result.error]
                    else:
                        truncated = result.error[:100]
                        error_lines = truncated.splitlines() or [truncated]

                    first_line, *remaining_lines = error_lines
                    print(f"    Run #{i+1}: {first_line}")
                    for line in remaining_lines:
                        print(f"      {line}")
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


def _resolve_git_metadata(start_dir: Path) -> Tuple[str, str]:
    """Return the current git branch name and revision hash.

    Falls back to ``"unknown"`` for each field if git is unavailable or the
    directory is not part of a repository.
    """

    def _run_git_command(*args: str) -> Optional[str]:
        try:
            completed = subprocess.run(
                ['git', *args],
                cwd=start_dir,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                check=True,
            )
        except (subprocess.SubprocessError, FileNotFoundError, OSError):
            return None

        value = completed.stdout.strip()
        return value or None

    repo_root = _run_git_command('rev-parse', '--show-toplevel')
    if repo_root:
        start_dir = Path(repo_root)

    branch = _run_git_command('rev-parse', '--abbrev-ref', 'HEAD') or 'unknown'
    if branch == 'HEAD':
        branch = 'detached HEAD'

    revision = _run_git_command('rev-parse', 'HEAD') or 'unknown'

    return branch, revision


def main():
    parser = argparse.ArgumentParser(
        description='Run Sintra tests with timeout and repetition support',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument('--repetitions', type=int, default=1,
                        help='Number of times to run each test (default: 1)')
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
                        choices=['Debug'],
                        help='Build configuration (default: Debug)')
    parser.add_argument('--verbose', action='store_true',
                        help='Show detailed output for each test run')
    parser.add_argument('--preserve-stalled-processes', action='store_true',
                        help='Leave stalled test processes running for debugging instead of terminating them')
    parser.add_argument('--kill_stalled_processes', action='store_true', help=argparse.SUPPRESS)

    args = parser.parse_args()

    # Resolve build directory
    script_dir = Path(__file__).parent
    print(f"{Color.BOLD}Sintra Test Runner{Color.RESET}")
    branch, revision = _resolve_git_metadata(script_dir)
    revision_display = revision if revision == 'unknown' else revision[:12]
    print(f"Git branch: {branch}")
    print(f"Git revision: {revision_display}")
    build_dir = (script_dir / args.build_dir).resolve()

    if not build_dir.exists():
        print(f"{Color.RED}Error: Build directory not found: {build_dir}{Color.RESET}")
        print(f"Please build the project first or specify correct --build-dir")
        return 1

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
                            'summary': first_line,
                            'message': error_message if error_message else first_line,
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
                        message = failure.get('message') or ''
                        summary = failure.get('summary') or message
                        lines = message.splitlines() if message else []

                        print(f"    Run #{failure['run']}: {summary}")

                        if lines:
                            if summary == lines[0]:
                                extra_lines = lines[1:]
                            else:
                                extra_lines = lines
                            for line in extra_lines:
                                print(f"      {line}")

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
