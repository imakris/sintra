#!/usr/bin/env python3
"""
Sintra Test Runner with Timeout Support

This script runs Sintra tests with proper timeout handling to detect
non-deterministic failures caused by OS scheduling issues.

Test selection and iteration counts are controlled by tests/active_tests.txt.

Usage:
    python run_tests.py [options]

Options:
    --timeout SECONDS               Timeout per test run in seconds (default: 5)
    --build-dir PATH                Path to build directory (default: ../build-ninja2)
    --config CONFIG                 Build configuration Debug/Release (default: Debug)
    --verbose                       Show detailed output for each test run
    --preserve-stalled-processes    Keep stalled processes running for debugging (default: terminate)
"""

import argparse
import contextlib
from functools import partial
import os
import shutil
import subprocess
import sys
import threading
import time
import uuid
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR.parent) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR.parent))

from typing import Any, Dict, IO, Iterable, Iterator, List, Optional, Sequence, Set, Tuple

from tests.debuggers import DebuggerStrategy, get_debugger_strategy
from tests.runner.configuration import (
    TEST_TIMEOUT_OVERRIDES,
    collect_compiler_metadata,
    parse_active_tests,
    print_configuration_build_details,
)
from tests.runner.platform import ManualSignalSupport, PlatformSupport, get_platform_support
from tests.runner.utils import (
    Color,
    available_disk_bytes,
    env_flag,
    find_lingering_processes,
    format_duration,
    format_size,
)

print = partial(__import__("builtins").print, file=sys.stderr, flush=True)


def _instrumentation_print(message: str) -> None:
    """Emit high-frequency instrumentation to stdout with explicit flushing."""

    sys.stdout.write(f"{message}\n")
    sys.stdout.flush()
    time.sleep(0.001)

try:
    import signal as _signal_module
except ImportError:
    _signal_module = None

_POSIX_STOP_SIGNALS: Set[int] = set()
if _signal_module is not None:
    for _sig_name in ("SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU"):
        _sig_value = getattr(_signal_module, _sig_name, None)
        if isinstance(_sig_value, int):
            _POSIX_STOP_SIGNALS.add(_sig_value)


PRESERVE_CORES_ENV = "SINTRA_PRESERVE_CORES"




# macOS emits Mach-O core files that snapshot every virtual memory region of the
# crashing process. Two platform effects make Sintra dumps look enormous even
# when very little physical memory is dirtied:
#
#   - The dynamic loader reserves a 4 GiB ``__PAGEZERO`` segment on every
#     process. That reservation is always recorded in the core image even though
#     it contains no data.
#   - Every request/reply ring that ``Managed_process`` touches is "double"
#     mapped by ``Ring_data::attach``: we reserve a fixed span and map the data
#     file twice so wrap-around reads stay linear. Each active channel therefore
#     contributes roughly 4 MiB of virtual address space. During the recovery
#     test the coordinator plus the watchdog/crasher pair keep dozens of these
#     channels open concurrently (outgoing rings for the local process plus
#     readers for every remote process slot), which adds roughly another 250 MiB
#     of reservations.
#
# GitHub Actions used to report the logical size of that 4 GiB + ~250 MiB address
# space (~4.24 GiB) as soon as a Mach-O core was written even though the rings
# are sparse files on APFS. ``MADV_DONTDUMP`` handles this automatically on
# Linux. macOS does not expose the flag, so ``recovery_test`` disables core
# dumps immediately before its intentional ``std::abort()`` to keep runners from
# filling their disks with Mach-O artifacts.

def _describe_processes(processes: Iterable[Tuple[int, str]]) -> str:
    """Return a compact string description of ``processes``."""

    formatted = [f"{name} (pid={pid})" for pid, name in processes]
    return ", ".join(formatted) if formatted else ""


# NOTE: Test iteration counts are now managed via active_tests.txt

# Tests that need extended timeouts beyond the global ``--timeout`` argument.
# Values represent the minimum timeout (in seconds) that should be enforced for
# the corresponding test invocation.
TEST_TIMEOUT_OVERRIDES = {
    "recovery_test_debug": 120.0,
    "recovery_test_release": 120.0,
    "barrier_complex_choreography_test": 120.0,
    "barrier_pathological_choreography_test": 120.0,
    "barrier_stress_test": 120.0,
    "crash_capture_self_test_debug": 120.0,
    "crash_capture_self_test_release": 120.0,
    "crash_capture_child_test_debug": 120.0,
    "crash_capture_child_test_release": 120.0,
}

STACK_CAPTURE_PREFIXES = ("crash_capture_",)
_STACK_CAPTURE_PAUSE_ENV = "SINTRA_CRASH_CAPTURE_PAUSE_MS"
_LIVE_STACK_ATTACH_TIMEOUT_ENV = "SINTRA_LIVE_STACK_ATTACH_TIMEOUT"

# Configure the maximum amount of wall time the runner spends attaching live
# debuggers before declaring stack capture unavailable. Users can extend this by
# setting ``SINTRA_LIVE_STACK_ATTACH_TIMEOUT`` (in seconds) when particularly
# heavy stress suites need more time.

def _canonical_test_name(name: str) -> str:
    """Return the canonical identifier used for weight lookups."""

    canonical = name.strip()
    if canonical.startswith("sintra_"):
        canonical = canonical[len("sintra_"):]
    if canonical.endswith("_adaptive"):
        canonical = canonical[: -len("_adaptive")]
    return canonical


def _strip_config_suffix(name: str) -> str:
    if name.endswith("_release") or name.endswith("_debug"):
        return name.rsplit("_", 1)[0]
    return name


def _is_stack_capture_test(name: str) -> bool:
    canonical = _canonical_test_name(name)
    if ':' in canonical:
        canonical = canonical.split(':', 1)[0]
    canonical = _strip_config_suffix(canonical)
    return canonical.startswith(STACK_CAPTURE_PREFIXES)


def _default_stack_capture_pause_ms() -> str:
    """Return a conservative pause window for stack-capture probes."""

    timeout_value = os.environ.get(_LIVE_STACK_ATTACH_TIMEOUT_ENV, "")
    attach_timeout = 0.0
    if timeout_value:
        try:
            attach_timeout = float(timeout_value)
        except ValueError:
            attach_timeout = 0.0

    if attach_timeout <= 0.0:
        attach_timeout = 90.0 if sys.platform == "darwin" else 30.0

    pause_seconds = max(2.0, min(10.0, attach_timeout * 0.1))
    return str(int(pause_seconds * 1000))


def _extract_debug_pause_pid(line: str) -> Optional[int]:
    """Extract the paused PID from a debug-pause log line."""

    for marker in ("Process ", "PID "):
        if marker not in line:
            continue
        tail = line.split(marker, 1)[1]
        digits = ""
        for ch in tail:
            if ch.isdigit():
                digits += ch
            elif digits:
                break
        if digits:
            try:
                return int(digits)
            except ValueError:
                return None
    return None


def _lookup_test_weight(name: str, active_tests: Dict[str, int]) -> int:
    """Return the repetition weight for the provided test invocation from active_tests.txt."""

    canonical = _canonical_test_name(name)

    # For ipc_rings_tests expanded invocations, look up the base test name
    # e.g., "ipc_rings_tests_release:unit:test_foo" -> "ipc_rings_tests"
    if ':' in canonical:
        # Extract base test name before first colon
        base_test = canonical.split(':')[0]
        if base_test in active_tests:
            return active_tests.get(base_test, 1)
        # Remove _release or _debug suffix
        if base_test.endswith('_release') or base_test.endswith('_debug'):
            base_test = base_test.rsplit('_', 1)[0]
        return active_tests.get(base_test, 1)

    # For regular tests, try with and without config suffix
    # e.g., "ping_pong_test_release" -> "ping_pong_test"
    if canonical.endswith('_release') or canonical.endswith('_debug'):
        if canonical in active_tests:
            return active_tests.get(canonical, 1)
        base_test = canonical.rsplit('_', 1)[0]
        return active_tests.get(base_test, 1)

    return active_tests.get(canonical, 1)


def _lookup_test_timeout(name: str, default: float) -> float:
    """Return the timeout for the provided test invocation."""

    canonical = _canonical_test_name(name)
    override = TEST_TIMEOUT_OVERRIDES.get(canonical)
    if override is None:
        return default
    return max(default, override)


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
        self.platform: PlatformSupport = get_platform_support(verbose=verbose)
        sanitized_config = ''.join(c.lower() if c.isalnum() else '_' for c in config)
        timestamp_ms = int(time.time() * 1000)
        scratch_dir_name = f".sintra-test-scratch-{sanitized_config}-{timestamp_ms}-{os.getpid()}"
        self._scratch_base = (self.build_dir / scratch_dir_name).resolve()
        self._scratch_base.mkdir(parents=True, exist_ok=True)
        self._scratch_lock = threading.Lock()
        self._scratch_counter = 0
        self._ipc_rings_cache: Dict[Path, List[Tuple[str, str]]] = {}
        self._stack_capture_history: Dict[str, Set[str]] = defaultdict(set)
        self._stack_capture_history_lock = threading.Lock()
        self._preserve_core_dumps = env_flag(PRESERVE_CORES_ENV)
        self._core_cleanup_lock = threading.Lock()
        self._core_cleanup_messages: List[Tuple[str, str]] = []
        self._core_cleanup_bytes_freed = 0
        self._scratch_cleanup_lock = threading.Lock()
        self._scratch_cleanup_dirs_removed = 0
        self._scratch_cleanup_bytes_freed = 0
        self._manual_stack_notice_printed = False
        self._instrumentation_enabled = True
        self._instrumentation_disk_root: Optional[Path] = self.build_dir
        self._instrument_disk_cache: Dict[Path, Tuple[float, str]] = {}

        # Help the Windows debugger locate private symbols (PDBs) for tests.
        # This is consumed by WindowsDebuggerStrategy when constructing the
        # .sympath command, and keeps CI stacks fully symbolized.
        if os.name == "nt":
            symbol_paths: List[str] = []
            # Per-configuration test binaries (e.g., build/tests/Release).
            symbol_paths.append(str(self.build_dir / "tests" / self.config))
            # Flat tests/ directory for single-config generators.
            symbol_paths.append(str(self.build_dir / "tests"))
            # Root build directory as a fallback.
            symbol_paths.append(str(self.build_dir))

            existing = os.environ.get("SINTRA_WINDOWS_SYMBOL_PATH", "")
            if existing:
                symbol_paths.append(existing)

            # Preserve order but drop duplicates.
            seen: Set[str] = set()
            merged_paths: List[str] = []
            for path in symbol_paths:
                if not path:
                    continue
                if path in seen:
                    continue
                seen.add(path)
                merged_paths.append(path)

            if merged_paths:
                os.environ["SINTRA_WINDOWS_SYMBOL_PATH"] = ";".join(merged_paths)

        # Preserve failing scratch directories when requested (useful for CI artifact capture).
        self._preserve_scratch = env_flag("SINTRA_PRESERVE_SCRATCH")
        self._debugger: DebuggerStrategy = get_debugger_strategy(
            self.verbose,
            print_fn=print,
            color=Color,
            collect_process_group_pids=self._collect_process_group_pids,
            collect_descendant_pids=self._collect_descendant_pids,
        )

        # Determine test directory - check both with and without config subdirectory
        test_dir_with_config = build_dir / 'tests' / config
        test_dir_simple = build_dir / 'tests'

        if test_dir_with_config.exists():
            self.test_dir = test_dir_with_config
        else:
            self.test_dir = test_dir_simple

        # Kill any existing sintra processes for a clean start
        self._kill_all_sintra_processes()

        self._debugger.prepare()

        dump_error = self._debugger.ensure_crash_dumps()
        if dump_error:
            print(
                f"{Color.YELLOW}Warning: {dump_error}. "
                f"Crash dumps may be unavailable.{Color.RESET}"
            )

        jit_error = self._debugger.configure_jit_debugging()
        if jit_error:
            print(
                f"{Color.YELLOW}Warning: {jit_error}. "
                f"JIT prompts may still block crash dumps.{Color.RESET}"
            )

    def instrumentation_active(self) -> bool:
        """Return True when verbose instrumentation should be emitted."""

        return self._instrumentation_enabled

    def _instrument_step(self, message: str, *, disk_path: Optional[Path] = None) -> None:
        if not self._instrumentation_enabled:
            return

        sample_path: Optional[Path]
        if disk_path is not None:
            sample_path = disk_path
        else:
            sample_path = self._instrumentation_disk_root

        suffix = ""
        cache_key: Optional[Path] = None
        if sample_path is not None:
            try:
                cache_key = sample_path.resolve()
            except Exception:
                cache_key = sample_path

        if cache_key is not None:
            now = time.monotonic()
            cached = self._instrument_disk_cache.get(cache_key)
            if cached and now - cached[0] < 0.5:
                suffix = cached[1]
            else:
                free_bytes = available_disk_bytes(cache_key)
                formatted = format_size(free_bytes)
                suffix = f" [disk free: {formatted} at {cache_key}]"
                self._instrument_disk_cache[cache_key] = (now, suffix)

        _instrumentation_print(f"{message}{suffix}")

    def _allocate_scratch_directory(self, invocation: TestInvocation) -> Path:
        """Create a per-invocation scratch directory for test artifacts."""

        safe_name = ''.join(c if c.isalnum() or c in ('-', '_') else '_' for c in invocation.name)
        with self._scratch_lock:
            index = self._scratch_counter
            self._scratch_counter += 1

        directory = self._scratch_base / f"{safe_name}_{index}"
        directory.mkdir(parents=True, exist_ok=True)
        return directory

    def _build_test_environment(self, invocation: TestInvocation, scratch_dir: Path) -> Dict[str, str]:
        """Return the environment variables for a test invocation."""

        env = os.environ.copy()
        env['SINTRA_TEST_ROOT'] = str(scratch_dir)
        # Route temp paths into the scratch directory so temp_directory_path()
        # stays under SINTRA_TEST_ROOT (avoids stale system temp collisions).
        env['TEMP'] = str(scratch_dir)
        env['TMP'] = str(scratch_dir)
        # Enable debug pause handlers for crash detection and debugger attachment
        env['SINTRA_DEBUG_PAUSE_ON_EXIT'] = '1'
        if _is_stack_capture_test(invocation.name):
            env['SINTRA_DEBUG_PAUSE_ON_EXIT'] = '0'
            env.setdefault(_STACK_CAPTURE_PAUSE_ENV, _default_stack_capture_pause_ms())
        return env

    @staticmethod
    def _estimate_directory_size(directory: Path) -> int:
        """Return a best-effort estimate of ``directory`` size in bytes."""

        total = 0
        if not directory.exists():
            return total

        try:
            for root, _, files in os.walk(directory):
                root_path = Path(root)
                for name in files:
                    file_path = root_path / name
                    try:
                        total += file_path.stat().st_size
                    except OSError:
                        continue
        except OSError:
            return total

        return total

    def _record_scratch_cleanup(self, freed_bytes: int) -> None:
        """Track scratch-directory cleanup statistics for later reporting."""

        with self._scratch_cleanup_lock:
            self._scratch_cleanup_dirs_removed += 1
            if freed_bytes > 0:
                self._scratch_cleanup_bytes_freed += freed_bytes

    def _cleanup_scratch_directory(self, directory: Path) -> None:
        """Best-effort removal of a scratch directory."""

        size_estimate = self._estimate_directory_size(directory)

        try:
            shutil.rmtree(directory)
        except FileNotFoundError:
            return
        except Exception as exc:
            print(
                f"\n{Color.YELLOW}Warning: Failed to remove scratch directory {directory}: {exc}{Color.RESET}"
            )
            return

        self._record_scratch_cleanup(size_estimate)

    def _core_dump_search_directories(self, invocation: TestInvocation) -> List[Path]:
        """Return directories that may contain core dumps for ``invocation``."""

        candidates: Set[Path] = {
            invocation.path.parent.resolve(),
            Path.cwd().resolve(),
            self.build_dir.resolve(),
            self._scratch_base,
        }

        enriched = self.platform.core_dump_directories(candidates)
        return [path for path in enriched if path]

    @staticmethod
    def _is_core_dump_file(path: Path) -> bool:
        """Return True if ``path`` appears to be a core dump file."""

        name = path.name
        return (
            name == "core"
            or name.startswith("core.")
            or name.startswith("core-")
            or name.endswith(".core")
        )

    @staticmethod
    def _normalize_core_path(path: Path) -> Path:
        """Return a canonical representation for core dump paths."""

        try:
            return path.resolve()
        except OSError:
            return path

    def _snapshot_core_dumps(self, invocation: TestInvocation) -> Set[Path]:
        """Capture the set of core dump files before launching a test."""

        snapshot: Set[Path] = set()
        for directory in self._core_dump_search_directories(invocation):
            try:
                entries = list(directory.iterdir())
            except OSError:
                continue

            for entry in entries:
                if not entry.is_file():
                    continue
                if not self._is_core_dump_file(entry):
                    continue
                snapshot.add(self._normalize_core_path(entry))

        return snapshot

    def _find_new_core_dumps(
        self,
        invocation: TestInvocation,
        snapshot: Set[Path],
        start_time: float,
    ) -> List[Tuple[Path, float, int]]:
        """Return newly created core dumps after a test run."""

        new_dumps: List[Tuple[Path, float, int]] = []
        for directory in self._core_dump_search_directories(invocation):
            try:
                entries = list(directory.iterdir())
            except OSError:
                continue

            for entry in entries:
                if not entry.is_file():
                    continue
                if not self._is_core_dump_file(entry):
                    continue

                normalized = self._normalize_core_path(entry)
                if normalized in snapshot:
                    continue

                try:
                    stat_info = entry.stat()
                except OSError:
                    continue

                if stat_info.st_mtime + 0.001 < start_time:
                    continue

                new_dumps.append((normalized, stat_info.st_mtime, stat_info.st_size))

        return new_dumps

    def _record_core_cleanup(self, level: str, message: str, freed_bytes: int = 0) -> None:
        """Record a core cleanup message to be emitted later."""

        with self._core_cleanup_lock:
            self._core_cleanup_messages.append((level, message))
            if freed_bytes:
                self._core_cleanup_bytes_freed += freed_bytes

    def _cleanup_new_core_dumps(
        self,
        invocation: TestInvocation,
        snapshot: Set[Path],
        start_time: float,
        result_success: Optional[bool],
    ) -> None:
        """Remove new core dumps to avoid exhausting disk space."""

        new_dumps = self._find_new_core_dumps(invocation, snapshot, start_time)
        if not new_dumps:
            return

        total_size = sum(size for _, _, size in new_dumps if size is not None)

        message_prefix = f"Core dumps ({invocation.name})"
        if result_success:
            message_prefix = f"{message_prefix} - test reported success"

        if self._preserve_core_dumps:
            message = (
                f"{message_prefix}: detected "
                f"{len(new_dumps)} file(s) totalling {format_size(total_size)}"
                f" (preserved due to {PRESERVE_CORES_ENV})"
            )
            level = "warning" if result_success else "info"
            self._record_core_cleanup(level, message)
            return

        removed: List[str] = []
        freed_bytes = 0
        errors: List[str] = []

        for path, _, size in new_dumps:
            try:
                if size is None:
                    size = 0
                path.unlink()
                removed.append(path.name)
                freed_bytes += size
            except OSError as exc:
                errors.append(f"Core dumps: failed to remove {path}: {exc}")

        if removed:
            removed.sort()
            message = (
                f"{message_prefix}: removed "
                f"{len(removed)} file(s) totalling {format_size(freed_bytes)}"
                f" ({', '.join(removed)})"
            )
            level = "warning" if result_success else "info"
            self._record_core_cleanup(level, message, freed_bytes=freed_bytes)
        else:
            # Nothing removed; still report total detected to aid diagnostics.
            message = (
                f"{message_prefix}: detected "
                f"{len(new_dumps)} file(s) totalling {format_size(total_size)}"
            )
            level = "warning" if result_success else "info"
            self._record_core_cleanup(level, message)

        for error in errors:
            self._record_core_cleanup("warning", error)

    def cleanup(self) -> None:
        """Remove the root scratch directory for this runner."""

        if self._preserve_scratch:
            print(
                f"{Color.YELLOW}Scratch preservation enabled (SINTRA_PRESERVE_SCRATCH=1); "
                f"leaving {self._scratch_base}{Color.RESET}"
            )
            return

        self._cleanup_scratch_directory(self._scratch_base)

    def consume_core_cleanup_reports(self) -> Tuple[int, List[Tuple[str, str]]]:
        """Return accumulated core cleanup messages and reset the state."""

        with self._core_cleanup_lock:
            freed = self._core_cleanup_bytes_freed
            messages = list(self._core_cleanup_messages)
            self._core_cleanup_messages.clear()
            self._core_cleanup_bytes_freed = 0

        with self._scratch_cleanup_lock:
            scratch_dirs = self._scratch_cleanup_dirs_removed
            scratch_bytes = self._scratch_cleanup_bytes_freed
            self._scratch_cleanup_dirs_removed = 0
            self._scratch_cleanup_bytes_freed = 0

        total_freed = freed + scratch_bytes

        if scratch_dirs:
            noun = "directory" if scratch_dirs == 1 else "directories"
            messages.append(
                (
                    "info",
                    (
                        "Scratch: removed "
                        f"{scratch_dirs} {noun} totalling {format_size(scratch_bytes)}"
                    ),
                )
            )

        return total_freed, messages

    def find_test_suites(self, active_tests: Dict[str, int]) -> Tuple[Dict[str, List[TestInvocation]], Dict[str, int]]:
        """Find test executables specified in active_tests.txt.

        Returns:
            Tuple of (test_suites, active_tests) where:
            - test_suites: Dict mapping config name to list of TestInvocations
            - active_tests: Dict mapping test paths to iteration counts
        """
        if not self.test_dir.exists():
            print(f"{Color.RED}Test directory not found: {self.test_dir}{Color.RESET}")
            return {}, active_tests

        if not active_tests:
            print(f"{Color.YELLOW}No tests specified in active_tests.txt{Color.RESET}")
            return {}, active_tests

        # 2 configurations: debug and release
        configurations = ['debug', 'release']

        # Map to store discovered test executables by base name
        # Key: base test name (e.g., "ping_pong_test")
        # Value: dict of {config: Path}
        available_tests: Dict[str, Dict[str, Path]] = {}

        try:
            directory_entries = sorted(self.test_dir.iterdir())
        except OSError as exc:
            print(f"{Color.RED}Failed to inspect test directory {self.test_dir}: {exc}{Color.RESET}")
            return {}, active_tests

        # Scan for available test binaries
        for entry in directory_entries:
            if not (entry.is_file() or entry.is_symlink()):
                continue

            normalized_name = self.platform.adjust_executable_name(entry.name)

            # Check if this matches a test with config suffix
            for config in configurations:
                suffix = f"_{config}"
                if normalized_name.endswith(suffix):
                    base_name = normalized_name[:-len(suffix)]
                    # Remove "sintra_" prefix if present
                    if base_name.startswith("sintra_"):
                        base_name = base_name[len("sintra_"):]

                    if base_name not in available_tests:
                        available_tests[base_name] = {}
                    available_tests[base_name][config] = entry
                    break

        # Now match active_tests entries with available binaries
        discovered_tests: Dict[str, List[TestInvocation]] = {
            config: [] for config in configurations
        }

        for test_path in active_tests.keys():
            # Extract test name from path (e.g., "manual/some_test" -> "some_test")
            test_name = test_path.split('/')[-1]
            target_config = None

            if test_name.endswith('_release') or test_name.endswith('_debug'):
                test_name, target_config = test_name.rsplit('_', 1)

            if test_name not in available_tests:
                print(f"{Color.YELLOW}Warning: Test '{test_path}' from active_tests.txt not found in build directory{Color.RESET}")
                continue

            # Add test invocations for each available configuration
            for config, test_binary in available_tests[test_name].items():
                if target_config is not None and config != target_config:
                    continue
                normalized_name = self.platform.adjust_executable_name(test_binary.name)

                invocations = self._expand_test_invocations(test_binary, f"sintra_{test_name}", normalized_name)
                if invocations:
                    discovered_tests[config].extend(invocations)

        # Build final test suites with ordering (dummy_test first)
        test_suites = {}
        for config, tests in discovered_tests.items():
            if not tests:
                continue
            ordered = sorted(
                enumerate(tests),
                key=lambda item: (
                    0 if _canonical_test_name(item[1].name).startswith("dummy_test") else 1,
                    _canonical_test_name(item[1].name),
                ),
            )
            test_suites[config] = [invocation for _, invocation in ordered]

        if not test_suites:
            print(f"{Color.RED}No test binaries found matching active_tests.txt{Color.RESET}")
            return {}, active_tests

        return test_suites, active_tests


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
            stderr_output = (result.stderr or "").strip()
            stdout_output = (result.stdout or "").strip()
            print(
                f"{Color.YELLOW}Warning: Failed to list tests for {entry.name} (exit {result.returncode}). "
                f"stderr: {stderr_output or stdout_output}{Color.RESET}"
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
        timeout = _lookup_test_timeout(invocation.name, self.timeout)
        run_id = uuid.uuid4().hex[:8]
        scratch_dir = self._allocate_scratch_directory(invocation)
        process = None
        cleanup_scratch_dir = True
        core_snapshot = self._snapshot_core_dumps(invocation)
        start_time = time.time()
        result_success: Optional[bool] = None
        instrumentation_active = self.instrumentation_active()
        instrument = partial(self._instrument_step, disk_path=self.build_dir)
        with self._stack_capture_history_lock:
            prefix = f"{invocation.name}:"
            keys_to_clear = [
                key
                for key in self._stack_capture_history.keys()
                if key == invocation.name or key.startswith(prefix)
            ]
            for key in keys_to_clear:
                self._stack_capture_history.pop(key, None)
        try:
            popen_env = self._build_test_environment(invocation, scratch_dir)
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

            self.platform.configure_popen(popen_kwargs)
            popen_kwargs['env'] = popen_env

            stdout_lines: List[str] = []
            stderr_lines: List[str] = []
            live_stack_traces = ""
            live_stack_error = ""
            postmortem_stack_traces = ""
            postmortem_stack_error = ""
            self_stack_detected = False
            failure_event = threading.Event()
            hang_detected = False
            hang_notes: List[str] = []
            capture_lock = threading.Lock()
            capture_pause_total = 0.0
            capture_active_start: Optional[float] = None
            threads: List[Tuple[threading.Thread, IO[str], str, Optional[int]]] = []
            reader_state_lock = threading.Lock()
            reader_states: Dict[str, Dict[str, Any]] = {}
            process_group_id: Optional[int] = None
            manual_capture_event = threading.Event()
            manual_capture_stop = threading.Event()
            manual_capture_thread: Optional[threading.Thread] = None
            manual_signal_handlers: Dict[int, object] = {}
            manual_signal_module: Optional[object] = None
            stop_signal_desc: Optional[str] = None
            posix_signal: Optional[int] = None
            heartbeat_stop = threading.Event()
            heartbeat_thread: Optional[threading.Thread] = None

            @contextlib.contextmanager
            def stack_capture_context(mark_failure: bool) -> Iterator[bool]:
                nonlocal capture_pause_total, capture_active_start

                capture_started = time.monotonic()
                with capture_lock:
                    if capture_active_start is not None:
                        yield False
                        return
                    if mark_failure:
                        if failure_event.is_set():
                            yield False
                            return
                        failure_event.set()
                    capture_active_start = capture_started

                try:
                    yield True
                finally:
                    capture_finished = time.monotonic()
                    with capture_lock:
                        if capture_active_start is not None:
                            capture_pause_total += capture_finished - capture_active_start
                            capture_active_start = None

            def terminate_process_group_members(reason: str) -> bool:
                """Ensure helpers in the spawned process group terminate."""

                pgid = process_group_id
                if pgid is None:
                    return False

                try:
                    base_exclusions = {0, os.getpid()}
                except Exception:
                    base_exclusions = {0}

                if process is not None and process.pid is not None:
                    base_exclusions.add(process.pid)

                descendant_candidates: Set[int] = set()
                if process is not None and process.pid is not None:
                    descendant_candidates = {
                        pid
                        for pid in self._collect_descendant_pids(process.pid)
                        if pid not in base_exclusions
                    }

                survivors = set(
                    pid
                    for pid in self._collect_process_group_pids(pgid)
                    if pid not in base_exclusions
                )

                if not survivors:
                    return False

                survivor_list = sorted(survivors)

                try:
                    import signal
                except ImportError:
                    return bool(survivors)

                for target_pid in survivor_list:
                    try:
                        os.kill(target_pid, signal.SIGTERM)
                    except ProcessLookupError:
                        continue
                    except PermissionError as exc:
                        pass
                    except OSError as exc:
                        pass

                wait_deadline = time.monotonic() + 1.0
                while time.monotonic() < wait_deadline:
                    survivors = set(
                        pid
                        for pid in self._collect_process_group_pids(pgid)
                        if pid not in base_exclusions
                    )
                    if not survivors:
                        return False
                    time.sleep(0.05)

                survivors = set(
                    pid
                    for pid in self._collect_process_group_pids(pgid)
                    if pid not in base_exclusions
                )

                if not survivors:
                    return False

                survivor_list = sorted(survivors)

                for target_pid in survivor_list:
                    try:
                        os.kill(target_pid, signal.SIGKILL)
                    except ProcessLookupError:
                        continue
                    except PermissionError as exc:
                        pass
                    except OSError as exc:
                        pass

                time.sleep(0.05)

                survivors = set(
                    pid
                    for pid in self._collect_process_group_pids(pgid)
                    if pid not in base_exclusions
                )


                return bool(survivors)

            def shutdown_reader_threads() -> None:
                """Ensure log reader threads terminate to avoid leaking resources."""
                join_step = 0.2
                max_join_time = 5.0

                def snapshot_reader_state(thread_name: str) -> str:
                    with reader_state_lock:
                        state = dict(reader_states.get(thread_name, {}))
                    descriptor = state.get("descriptor", "unknown")
                    lines = state.get("lines") or 0
                    bytes_read = state.get("bytes") or 0
                    last_update = state.get("last_update")
                    start_time_state = state.get("start_time")
                    lifetime_str = "unknown"
                    if isinstance(start_time_state, float) and isinstance(last_update, float):
                        lifetime = max(last_update - start_time_state, 0.0)
                        lifetime_str = f"{lifetime:.3f}s"
                    if isinstance(last_update, float):
                        idle = max(time.monotonic() - last_update, 0.0)
                        last_update_str = f"{idle:.3f}s ago"
                    else:
                        last_update_str = "unknown"
                    active = state.get("active")
                    last_line = state.get("last_line_excerpt")
                    excerpt = repr(last_line) if last_line is not None else "None"
                    return (
                        f"descriptor={descriptor} lines={lines} bytes={bytes_read} runtime={lifetime_str} "
                        f"last_activity={last_update_str} active={active} last_line={excerpt}"
                    )

                for thread, stream, descriptor, stream_fd in threads:
                    join_deadline = time.monotonic() + max_join_time
                    while thread.is_alive():
                        remaining = join_deadline - time.monotonic()
                        if remaining <= 0:
                            break
                        join_timeout = min(join_step, remaining)
                        thread.join(timeout=join_timeout)

                    if thread.is_alive():
                        terminate_process_group_members(
                            f"log reader stall ({descriptor}) for {invocation.name}"
                        )
                        forcible_close_applied = False
                        if stream_fd is not None:
                            try:
                                os.close(stream_fd)
                                forcible_close_applied = True
                            except OSError as exc:
                                pass
                        if not forcible_close_applied:
                            try:
                                stream.close()
                            except Exception:
                                pass
                        thread.join(timeout=join_step)

                    if thread.is_alive():
                        print(
                            f"\n{Color.YELLOW}Warning: Log reader thread did not terminate cleanly; "
                            "closing descriptors to avoid resource leak.{Color.RESET}"
                        )


            process = subprocess.Popen(invocation.command(), **popen_kwargs)

            if hasattr(os, 'getpgid'):
                try:
                    process_group_id = os.getpgid(process.pid)
                except (ProcessLookupError, PermissionError, OSError):
                    process_group_id = None

            # Hard safety watchdog: absolute deadline that will force-kill the process
            # if the normal timeout mechanism fails for any reason. This is a last-resort
            # safety net to prevent infinite hangs.
            hard_deadline = time.monotonic() + timeout + 60.0  # Normal timeout + 60s grace
            hard_watchdog_stop = threading.Event()

            def hard_watchdog() -> None:
                while not hard_watchdog_stop.wait(1.0):
                    if time.monotonic() >= hard_deadline:
                        if process.poll() is None:
                            print(
                                f"\n{Color.RED}HARD WATCHDOG: Force-killing {invocation.name} "
                                f"(pid {process.pid}) - normal timeout mechanism failed{Color.RESET}",
                                flush=True,
                            )
                            self._kill_process_tree(process.pid)
                        else:
                            # Main process exited but we might be stuck on reader threads
                            # waiting for child processes. Kill any lingering sintra processes.
                            lingering = find_lingering_processes(("sintra_",))
                            if lingering:
                                pids = [pid for pid, _ in lingering]
                                print(
                                    f"\n{Color.RED}HARD WATCHDOG: Main process exited but found "
                                    f"{len(lingering)} lingering child process(es): {pids} - killing them{Color.RESET}",
                                    flush=True,
                                )
                                for pid, _ in lingering:
                                    try:
                                        self._kill_process_tree(pid)
                                    except Exception:
                                        pass
                        break

            hard_watchdog_thread = threading.Thread(target=hard_watchdog, daemon=True)
            hard_watchdog_thread.start()

            def attempt_live_capture(trigger_line: str) -> None:
                nonlocal live_stack_traces, live_stack_error
                if not trigger_line:
                    return

                # Check for SINTRA_DEBUG_PAUSE marker (process has paused for debugger attachment)
                if '[SINTRA_DEBUG_PAUSE]' in trigger_line and 'paused:' in trigger_line:
                    # Process has hit a crash and is paused - capture immediately
                    target_pid = _extract_debug_pause_pid(trigger_line)
                    if target_pid is None and process is not None:
                        target_pid = process.pid
                    if not self._should_attempt_stack_capture(
                        invocation,
                        'debug_pause',
                        target_pid=target_pid,
                    ):
                        return

                    traces = ""
                    error = ""
                    with stack_capture_context(mark_failure=False) as allowed:
                        if not allowed:
                            return
                        if target_pid is None:
                            return
                        traces, error = self._capture_process_stacks(
                            target_pid,
                            process_group_id,
                        )

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

                    # Allow manual attachment for regular tests; stack-capture
                    # tests disable debug pause so they can terminate normally.

                    return

                # Regular failure marker detection
                if not self._line_indicates_failure(trigger_line):
                    return
                if not self._should_attempt_stack_capture(invocation, 'live_failure'):
                    return

                traces = ""
                error = ""
                with stack_capture_context(mark_failure=True) as allowed:
                    if not allowed:
                        return
                    traces, error = self._capture_process_stacks(
                        process.pid,
                        process_group_id,
                    )

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

            manual_signal_registered = False

            manual_signal_info: Optional[ManualSignalSupport] = self.platform.manual_signal_support()
            if manual_signal_info is not None:
                manual_signal_module = manual_signal_info.module
                manual_signal = manual_signal_info.signal_number

                try:
                    signal_label = manual_signal_module.Signals(manual_signal).name  # type: ignore[attr-defined]
                except Exception:
                    signal_label = f"signal {manual_signal}"

                def _manual_signal_handler(signum, frame):  # type: ignore[override]
                    manual_capture_event.set()

                previous = manual_signal_module.getsignal(manual_signal)
                manual_signal_handlers[manual_signal] = previous
                manual_signal_module.signal(manual_signal, _manual_signal_handler)
                manual_signal_registered = True

                if self.verbose and not self._manual_stack_notice_printed:
                    print(
                        f"{Color.BLUE}Manual stack capture enabled: send {signal_label} to PID {os.getpid()} "
                        f"({invocation.name}) to snapshot live stacks.{Color.RESET}",
                        flush=True,
                    )
                    self._manual_stack_notice_printed = True

                    def _manual_capture_worker() -> None:
                        nonlocal live_stack_traces, live_stack_error, capture_active_start

                        while not manual_capture_stop.is_set():
                            triggered = manual_capture_event.wait(timeout=0.2)
                            if not triggered:
                                continue
                            manual_capture_event.clear()
                            if manual_capture_stop.is_set():
                                break
                            if process.poll() is not None:
                                print(
                                    f"{Color.YELLOW}Manual stack capture requested but the test process has already "
                                    f"exited.{Color.RESET}",
                                    flush=True,
                                )
                                continue

                            with capture_lock:
                                active = capture_active_start is not None

                            if active:
                                print(
                                    f"{Color.YELLOW}Manual stack capture skipped: another capture is already in progress.{Color.RESET}",
                                    flush=True,
                                )
                                continue

                            timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime())
                            print(
                                f"{Color.BLUE}Manual stack capture started at {timestamp} (PID {process.pid}).{Color.RESET}",
                                flush=True,
                            )

                            traces = ""
                            error = ""
                            with stack_capture_context(mark_failure=False) as allowed:
                                if not allowed:
                                    print(
                                        f"{Color.YELLOW}Manual stack capture skipped: capture context unavailable.{Color.RESET}",
                                        flush=True,
                                    )
                                    continue
                                traces, error = self._capture_process_stacks(
                                    process.pid,
                                    process_group_id,
                                )

                            with capture_lock:
                                if traces:
                                    header = f"Manual stack capture at {timestamp}"
                                    if live_stack_traces:
                                        live_stack_traces = f"{live_stack_traces}\n\n{header}\n{traces}"
                                    else:
                                        live_stack_traces = f"{header}\n{traces}"
                                    live_stack_error = ""
                                elif error and not live_stack_traces:
                                    live_stack_error = error

                            if traces:
                                print(
                                    f"{Color.BLUE}Manual stack capture completed (PID {process.pid}).{Color.RESET}",
                                    flush=True,
                                )
                            elif error:
                                print(
                                    f"{Color.YELLOW}Manual stack capture failed: {error}{Color.RESET}",
                                    flush=True,
                                )

                        manual_capture_event.clear()

                    manual_capture_thread = threading.Thread(
                        target=_manual_capture_worker,
                        name=f"manual_stack_capture_{process.pid}",
                        daemon=True,
                    )
                    manual_capture_thread.start()

            def monitor_stream(stream, buffer: List[str], descriptor: str) -> None:
                nonlocal self_stack_detected
                thread_name = threading.current_thread().name
                start_time = time.monotonic()
                lines = 0
                bytes_read = 0
                with reader_state_lock:
                    reader_states[thread_name] = {
                        "descriptor": descriptor,
                        "lines": 0,
                        "bytes": 0,
                        "last_update": start_time,
                        "active": True,
                        "last_line_excerpt": None,
                        "start_time": start_time,
                    }
                try:
                    for line in iter(stream.readline, ''):
                        buffer.append(line)
                        lines += 1
                        bytes_read += len(line.encode('utf-8', errors='ignore'))
                        now_monotonic = time.monotonic()
                        stripped = line.rstrip('\n')
                        if len(stripped) > 200:
                            stripped = f"{stripped[:197]}..."
                        with reader_state_lock:
                            entry = reader_states.setdefault(
                                thread_name,
                                {
                                    "descriptor": descriptor,
                                    "lines": 0,
                                    "bytes": 0,
                                    "last_update": start_time,
                                    "active": True,
                                    "last_line_excerpt": None,
                                    "start_time": start_time,
                                },
                            )
                            entry.update(
                                {
                                    "lines": lines,
                                    "bytes": bytes_read,
                                    "last_update": now_monotonic,
                                    "active": True,
                                    "last_line_excerpt": stripped,
                                }
                            )
                        if "[SINTRA_SELF_STACK_BEGIN]" in line:
                            self_stack_detected = True
                        attempt_live_capture(line)
                except (OSError, ValueError):
                    # The stream may be closed from another thread during shutdown.
                    pass
                except Exception:
                    # Swallow unexpected reader errors so they don't crash the runner.
                    pass
                finally:
                    try:
                        stream.close()
                    except Exception:
                        pass
                    end_time = time.monotonic()
                    with reader_state_lock:
                        entry = reader_states.setdefault(
                            thread_name,
                            {
                                "descriptor": descriptor,
                                "lines": 0,
                                "bytes": 0,
                                "last_update": start_time,
                                "active": False,
                                "last_line_excerpt": None,
                                "start_time": start_time,
                            },
                        )
                        entry.update(
                            {
                                "lines": lines,
                                "bytes": bytes_read,
                                "last_update": end_time,
                                "active": False,
                            }
                        )

            if process.stdout:
                stdout_fd: Optional[int] = None
                try:
                    stdout_fd = process.stdout.fileno()
                except (OSError, ValueError, AttributeError):
                    stdout_fd = None
                stdout_thread = threading.Thread(
                    target=monitor_stream,
                    args=(process.stdout, stdout_lines, 'stdout'),
                    daemon=True,
                )
                stdout_thread.start()
                threads.append((stdout_thread, process.stdout, 'stdout', stdout_fd))

            if process.stderr:
                stderr_fd: Optional[int] = None
                try:
                    stderr_fd = process.stderr.fileno()
                except (OSError, ValueError, AttributeError):
                    stderr_fd = None
                stderr_thread = threading.Thread(
                    target=monitor_stream,
                    args=(process.stderr, stderr_lines, 'stderr'),
                    daemon=True,
                )
                stderr_thread.start()
                threads.append((stderr_thread, process.stderr, 'stderr', stderr_fd))

            def _snapshot_reader_states() -> Dict[str, Dict[str, Any]]:
                with reader_state_lock:
                    return {k: dict(v) for k, v in reader_states.items()}

            def _summarize_descriptor(snapshot: Dict[str, Dict[str, Any]], descriptor: str) -> str:
                lines_read = 0
                last_line = None
                last_idle: Optional[float] = None
                for state in snapshot.values():
                    if state.get("descriptor") != descriptor:
                        continue
                    lines_read = max(lines_read, state.get("lines") or 0)
                    if state.get("last_line_excerpt") is not None:
                        last_line = state.get("last_line_excerpt")
                    last_update = state.get("last_update")
                    if isinstance(last_update, float):
                        last_idle = max(time.monotonic() - last_update, 0.0)
                idle_str = f"{last_idle:.2f}s" if last_idle is not None else "unknown"
                last_line_str = repr(last_line) if last_line is not None else "None"
                return f"lines={lines_read} idle={idle_str} last={last_line_str}"

            def _heartbeat() -> None:
                if not self.verbose:
                    # Heartbeat output is only enabled in verbose mode; in
                    # non-verbose runs this thread stays idle until teardown.
                    heartbeat_stop.wait()
                    return
                last_report = 0.0
                while not heartbeat_stop.wait(1.0):
                    if process.poll() is not None:
                        continue
                    elapsed = time.monotonic() - start_monotonic
                    # First heartbeat after 5s, then every ~10s to avoid log spam.
                    if elapsed < 5.0 or elapsed - last_report < 10.0:
                        continue
                    last_report = elapsed
                    snapshot = _snapshot_reader_states()
                    stdout_summary = _summarize_descriptor(snapshot, "stdout")
                    stderr_summary = _summarize_descriptor(snapshot, "stderr")
                    stdout_len = sum(len(s) for s in stdout_lines)
                    stderr_len = sum(len(s) for s in stderr_lines)
                    stderr_tail = stderr_lines[-5:]
                    if stderr_tail:
                        stderr_tail = [line.rstrip()[:400] for line in stderr_tail]
                    descendants = (
                        self._collect_descendant_pids(process.pid)
                        if (self.verbose and process.pid)
                        else []
                    )
                    details = (
                        self._describe_pids([process.pid] + descendants)
                        if (self.verbose and process.pid)
                        else {}
                    )
                    if self.verbose:
                        msg = (
                            f"[HEARTBEAT] {invocation.name} run_id={run_id} pid={process.pid} "
                            f"elapsed={elapsed:.1f}s timeout={timeout}s "
                            f"stdout={{ {stdout_summary} }} stderr={{ {stderr_summary} }} "
                            f"stdout_len={stdout_len} stderr_len={stderr_len} "
                        )
                        if stderr_tail:
                            msg += f"stderr_tail={stderr_tail} "
                        msg += f"descendants={descendants} scratch={scratch_dir}"
                        print(msg, flush=True)
                        if details:
                            for pid, desc in details.items():
                                print(f"[HEARTBEAT]    pid={pid} {desc}", flush=True)
                    else:
                        msg = (
                            f"[HEARTBEAT] {invocation.name} run_id={run_id} pid={process.pid} "
                            f"elapsed={elapsed:.1f}s timeout={timeout}s "
                            f"stdout={{ {stdout_summary} }} stderr={{ {stderr_summary} }} "
                            f"stdout_len={stdout_len} stderr_len={stderr_len}"
                        )
                        if stderr_tail:
                            msg += f" stderr_tail={stderr_tail}"
                        print(msg, flush=True)

            heartbeat_thread = threading.Thread(
                target=_heartbeat,
                name=f"heartbeat_{invocation.name}_{run_id}",
                daemon=True,
            )
            heartbeat_thread.start()

            # Wait with timeout, extending the deadline for live stack captures
            try:
                while True:
                    with capture_lock:
                        pause_total = capture_pause_total
                        active_start = capture_active_start

                    now_monotonic = time.monotonic()
                    active_extra = 0.0
                    if active_start is not None:
                        active_extra = now_monotonic - active_start

                    adjusted_deadline = (
                        start_monotonic + timeout + pause_total + active_extra
                    )

                    if process.poll() is not None:
                        break

                    remaining = adjusted_deadline - now_monotonic
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(process.args, timeout)

                    try:
                        process.wait(timeout=remaining)
                        break
                    except subprocess.TimeoutExpired:
                        continue

                process.wait()

                posix_signal = self._decode_posix_signal(process.returncode)
                if self._is_stop_signal(posix_signal):
                    stop_signal_desc = self._format_signal_name(posix_signal or 0)
                    reason = f"stop_signal:{posix_signal}"
                    if self._should_attempt_stack_capture(invocation, reason):
                        traces = ""
                        error = ""
                        with stack_capture_context(mark_failure=True) as allowed:
                            if allowed:
                                traces, error = self._capture_process_stacks(
                                    process.pid,
                                    process_group_id,
                                )
                        if traces:
                            if live_stack_traces:
                                live_stack_traces = f"{live_stack_traces}\n\n{traces}"
                            else:
                                live_stack_traces = traces
                            live_stack_error = ""
                        elif error and not live_stack_traces:
                            live_stack_error = error

                terminate_process_group_members(f"{invocation.name} exit")

                # On Windows, terminate_process_group_members does nothing useful because
                # there's no POSIX process group concept. Kill any lingering child processes
                # by name pattern to prevent reader threads from blocking on their pipes.
                if self.platform.is_windows:
                    prefixes = ("sintra_", invocation.path.stem, invocation.name)
                    lingering = find_lingering_processes(prefixes)
                    # Give recently-started children a brief window to exit naturally
                    # before classifying them as lingering. This avoids flagging short,
                    # in-flight shutdown phases as hangs while still catching processes
                    # that survive well beyond the test's lifetime.
                    if lingering:
                        grace_deadline = time.time() + 2.0
                        while lingering and time.time() < grace_deadline:
                            time.sleep(0.05)
                            lingering = find_lingering_processes(prefixes)
                    if lingering or self.verbose:
                        print(
                            f"[DEBUG] Found {len(lingering)} lingering processes: {lingering}",
                            flush=True,
                        )
                    lingering_details = self._describe_pids([pid for pid, _ in lingering]) if lingering else {}
                    if lingering:
                        for pid, name in lingering:
                            detail = lingering_details.get(pid)
                            if detail:
                                print(f"[DEBUG] Lingering process detail pid={pid} name={name}: {detail}", flush=True)
                            # Try to capture stacks before killing to understand why it is stuck.
                            try:
                                stacks, err = self._capture_process_stacks(pid, None)
                                if stacks:
                                    print(f"[DEBUG] Lingering process stacks pid={pid}:\n{stacks}", flush=True)
                                    hang_notes.append(f"linger pid={pid} stacks captured")
                                elif err:
                                    print(f"[DEBUG] Lingering process stack capture failed pid={pid}: {err}", flush=True)
                                    hang_notes.append(f"linger pid={pid} stack capture failed: {err}")
                            except Exception as e:
                                print(f"[DEBUG] Failed to capture stacks for pid={pid}: {e}", flush=True)
                            print(f"[DEBUG] Killing lingering process {pid} ({name})", flush=True)
                            try:
                                self._kill_process_tree(pid)
                                print(f"[DEBUG] Successfully killed {pid}", flush=True)
                            except Exception as e:
                                print(f"[DEBUG] Failed to kill {pid}: {e}", flush=True)
                        hang_detected = True
                    # Also look for children of this test process that may not match prefixes.
                    if process and process.pid:
                        descendants = self._collect_descendant_pids(process.pid)
                        if descendants:
                            details = self._describe_pids(descendants)
                            # Limit heavy-weight debugger work to descendants that look
                            # like Sintra/test processes. Windows CI sometimes reports
                            # a large number of unrelated descendants; attaching to
                            # and killing arbitrary system processes is both fragile
                            # and unnecessary.
                            interesting: List[int] = []
                            for pid in descendants:
                                detail = details.get(pid, "")
                                if any(
                                    prefix
                                    and prefix in detail
                                    for prefix in prefixes
                                ):
                                    interesting.append(pid)

                            if interesting:
                                print(
                                    f"[DEBUG] Descendants of {process.pid} still alive after exit: {interesting}",
                                    flush=True,
                                )
                                hang_detected = True
                                hang_notes.append(f"descendants after exit: {interesting}")
                                for pid in interesting:
                                    detail = details.get(pid)
                                    if detail:
                                        print(
                                            f"[DEBUG] Descendant detail pid={pid}: {detail}",
                                            flush=True,
                                        )
                                    try:
                                        stacks, err = self._capture_process_stacks(pid, None)
                                        if stacks:
                                            print(
                                                f"[DEBUG] Descendant stacks pid={pid}:\n{stacks}",
                                                flush=True,
                                            )
                                            hang_notes.append(
                                                f"descendant pid={pid} stacks captured"
                                            )
                                        elif err:
                                            print(
                                                f"[DEBUG] Descendant stack capture failed pid={pid}: {err}",
                                                flush=True,
                                            )
                                            hang_notes.append(
                                                f"descendant pid={pid} stack capture failed: {err}"
                                            )
                                    except Exception as e:
                                        print(
                                            f"[DEBUG] Failed to capture stacks for descendant pid={pid}: {e}",
                                            flush=True,
                                        )
                                    try:
                                        self._kill_process_tree(pid)
                                        print(
                                            f"[DEBUG] Killed descendant pid={pid}",
                                            flush=True,
                                        )
                                    except Exception as e:
                                        print(
                                            f"[DEBUG] Failed to kill descendant pid={pid}: {e}",
                                            flush=True,
                                        )
                            else:
                                if self.verbose:
                                    print(
                                        f"[DEBUG] Descendants of {process.pid} still alive after exit "
                                        f"(no sintra/test descendants): {descendants}",
                                        flush=True,
                                    )

                duration = time.time() - start_time

                shutdown_reader_threads()

                stdout = ''.join(stdout_lines)
                stderr = ''.join(stderr_lines)

                success = (process.returncode == 0) and not hang_detected
                error_msg = stderr
                probe_missing_crash = False

                if hang_detected:
                    hang_summary = "; ".join(hang_notes) if hang_notes else "detected lingering/descendant processes"
                    error_msg = f"[HANG DETECTED] {hang_summary}\n{error_msg}"

                if _is_stack_capture_test(invocation.name) and process.returncode == 0 and not hang_detected:
                    probe_missing_crash = True
                    success = False
                    error_msg = (
                        "STACK CAPTURE PROBE DID NOT CRASH "
                        f"(exit code {process.returncode})\n{stderr}"
                    )

                if not success and not probe_missing_crash:
                    # Categorize failure type for better diagnostics
                    if stop_signal_desc:
                        error_msg = (
                            f"STOPPED: Received {stop_signal_desc} "
                            f"(exit code {process.returncode})\n{stderr}"
                        )
                    elif process.returncode < 0 or process.returncode > 128:
                        # Unix signal (negative) or Windows crash code (large positive like 0xC0000005)
                        error_msg = f"CRASH: Process terminated abnormally (exit code {process.returncode})\n{stderr}"
                    elif duration < 0.1:
                        # Exited almost immediately - likely crash or early abort
                        error_msg = f"EARLY EXIT: Process exited with code {process.returncode} after {duration:.3f}s (possible crash)\n{stderr}"
                    else:
                        # Normal test failure
                        error_msg = f"TEST FAILED: Exit code {process.returncode} after {duration:.2f}s\n{stderr}"

                    if not live_stack_traces and not live_stack_error:
                        if self._should_attempt_stack_capture(invocation, 'post_failure'):
                            traces, error = self._capture_process_stacks(
                                process.pid,
                                process_group_id,
                            )
                            if traces:
                                live_stack_traces = traces
                            elif error:
                                live_stack_error = error

                    if self._is_crash_exit(process.returncode):
                        if self._should_attempt_stack_capture(invocation, 'crash'):
                            traces, error = self._capture_process_stacks(
                                process.pid,
                                process_group_id,
                            )
                            if traces:
                                if live_stack_traces:
                                    live_stack_traces = f"{live_stack_traces}\n\n{traces}"
                                else:
                                    live_stack_traces = traces
                                live_stack_error = ""
                            elif error and not live_stack_traces:
                                live_stack_error = error

                        if self._should_attempt_stack_capture(invocation, 'postmortem'):
                            (
                                postmortem_stack_traces,
                                postmortem_stack_error,
                            ) = self._capture_core_dump_stack(invocation, start_time, process.pid)

                if live_stack_traces:
                    error_msg = f"{error_msg}\n\n=== Captured stack traces ===\n{live_stack_traces}"

                if postmortem_stack_traces:
                    error_msg = f"{error_msg}\n\n=== Post-mortem stack trace ===\n{postmortem_stack_traces}"
                elif postmortem_stack_error:
                    error_msg = f"{error_msg}\n\n[Post-mortem stack capture unavailable: {postmortem_stack_error}]"

                if live_stack_error and not live_stack_traces:
                    live_label = "Live stack capture unavailable"
                    if postmortem_stack_traces:
                        live_label += " (post-mortem captured)"
                    error_msg = f"{error_msg}\n\n[{live_label}: {live_stack_error}]"

                if _is_stack_capture_test(invocation.name):
                    if live_stack_traces or postmortem_stack_traces:
                        capture_note = "STACK CAPTURE: debugger/postmortem"
                    elif self_stack_detected:
                        capture_note = "STACK CAPTURE: self-trace"
                    else:
                        capture_note = "STACK CAPTURE: MISSING"

                    if error_msg:
                        error_msg = f"{error_msg}\n\n{capture_note}"
                    else:
                        error_msg = capture_note

                result_success = success
                return TestResult(
                    success=success,
                    duration=duration,
                    output=stdout,
                    error=error_msg
                )

            except subprocess.TimeoutExpired as e:
                duration = timeout

                if self.preserve_on_timeout:
                    print(f"\n{Color.RED}TIMEOUT: Process exceeded timeout of {timeout}s (PID {process.pid}). Preserving for debugging as requested.{Color.RESET}")
                    print(f"{Color.YELLOW}Attach a debugger to PID {process.pid} or terminate it manually when done.{Color.RESET}")
                    cleanup_scratch_dir = False
                    sys.exit(2)

                stack_traces = live_stack_traces
                stack_error = live_stack_error
                if process:
                    if self._should_attempt_stack_capture(invocation, 'timeout'):
                        extra_traces, extra_error = self._capture_process_stacks(
                            process.pid,
                            process_group_id,
                        )
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

                # On Windows, also kill any lingering child processes by name pattern
                if self.platform.is_windows:
                    prefixes = ("sintra_", invocation.path.stem, invocation.name)
                    lingering = find_lingering_processes(prefixes)
                    if lingering:
                        lingering_details = self._describe_pids([pid for pid, _ in lingering])
                        for pid, name in lingering:
                            detail = lingering_details.get(pid)
                            if detail:
                                print(f"[DEBUG] (timeout) lingering pid={pid} name={name} detail={detail}", flush=True)
                            try:
                                stacks, err = self._capture_process_stacks(pid, None)
                                if stacks:
                                    print(f"[DEBUG] (timeout) lingering stacks pid={pid}:\n{stacks}", flush=True)
                                elif err:
                                    print(f"[DEBUG] (timeout) lingering stack capture failed pid={pid}: {err}", flush=True)
                            except Exception:
                                pass
                            try:
                                self._kill_process_tree(pid)
                            except Exception:
                                pass

                try:
                    process.wait(timeout=1)
                except Exception:
                    pass

                shutdown_reader_threads()

                stdout = ''.join(stdout_lines)
                stderr = ''.join(stderr_lines)

                if stack_traces:
                    stderr = f"{stderr}\n\n=== Captured stack traces ===\n{stack_traces}"
                elif stack_error:
                    stderr = f"{stderr}\n\n[Stack capture unavailable: {stack_error}]"

                result_success = False
                return TestResult(
                    success=False,
                    duration=duration,
                    output=stdout,
                    error=f"TIMEOUT: Test exceeded {timeout}s and was terminated.\n{stderr}"
                )

        except Exception as e:
            if process:
                self._kill_process_tree(process.pid)
            shutdown_reader_threads()
            stdout = ''.join(stdout_lines) if stdout_lines else ""
            stderr = ''.join(stderr_lines) if stderr_lines else ""
            error_msg = f"Exception: {str(e)}"
            if stderr:
                error_msg = f"{error_msg}\n{stderr}"
            result_success = False
            return TestResult(
                success=False,
                duration=0,
                output=stdout,
                error=error_msg
            )
        finally:
            hard_watchdog_stop.set()
            hard_watchdog_thread.join(timeout=1.0)
            heartbeat_stop.set()
            if heartbeat_thread is not None:
                heartbeat_thread.join(timeout=1.0)
            manual_capture_stop.set()
            manual_capture_event.set()
            if manual_capture_thread is not None:
                manual_capture_thread.join(timeout=1.0)
            if manual_signal_registered and manual_signal_module is not None:
                for sig, prev_handler in manual_signal_handlers.items():
                    try:
                        manual_signal_module.signal(sig, prev_handler)  # type: ignore
                    except Exception:
                        pass

            preserve_failure = self._preserve_scratch and (result_success is False or result_success is None)
            if preserve_failure:
                cleanup_scratch_dir = False
                print(
                    f"\n{Color.YELLOW}Preserving scratch for failure ({invocation.name}): "
                    f"{scratch_dir}{Color.RESET}"
                )
            if hang_detected:
                cleanup_scratch_dir = False
                print(
                    f"\n{Color.YELLOW}Preserving scratch for hang ({invocation.name}): "
                    f"{scratch_dir}{Color.RESET}"
                )

            if cleanup_scratch_dir:
                self._cleanup_scratch_directory(scratch_dir)
            self._cleanup_new_core_dumps(invocation, core_snapshot, start_time, result_success)

    def _kill_process_tree(self, pid: int):
        """Kill a process and all its children"""
        try:
            self.platform.kill_process_tree(pid)
        except Exception as e:
            # Log but don't fail if cleanup fails
            print(f"\n{Color.YELLOW}Warning: Failed to kill process {pid}: {e}{Color.RESET}")
            pass

    def _kill_all_sintra_processes(self):
        """Kill all existing sintra processes to ensure clean start"""
        try:
            self.platform.kill_all_sintra_processes()
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

    def _should_attempt_stack_capture(
        self,
        invocation: TestInvocation,
        reason: str,
        target_pid: Optional[int] = None,
    ) -> bool:
        """Return True if stack capture for the invocation/reason has not run yet."""

        if reason == 'debug_pause' and target_pid is not None:
            key = f"{invocation.name}:pid{target_pid}"
        else:
            key = invocation.name
        with self._stack_capture_history_lock:
            attempted = self._stack_capture_history[key]
            if reason in attempted:
                return False
            attempted.add(reason)
            return True

    def _is_crash_exit(self, returncode: int) -> bool:
        """Return True if the exit code represents an abnormal termination."""

        if returncode == 0:
            return False

        if self.platform.is_windows:
            # Windows crash codes are typically large unsigned values such as 0xC0000005.
            return returncode < 0 or returncode >= 0xC0000000

        signal_num = self._decode_posix_signal(returncode)
        if signal_num is not None and signal_num in _POSIX_STOP_SIGNALS:
            return False

        # POSIX: negative return codes indicate termination by signal.
        # Codes > 128 are also commonly used to report signal-based exits.
        return returncode < 0 or returncode > 128

    def _decode_posix_signal(self, returncode: int) -> Optional[int]:
        """Return the POSIX signal number encoded in a return code."""

        if returncode == 0 or self.platform.is_windows:
            return None

        if returncode < 0:
            return -returncode

        if returncode > 128:
            signal_num = returncode - 128
            if signal_num > 0:
                return signal_num
        return None

    @staticmethod
    def _format_signal_name(signal_num: int) -> str:
        if _signal_module is not None:
            try:
                return _signal_module.Signals(signal_num).name
            except Exception:
                pass
        return f"signal {signal_num}"

    def _capture_process_stacks(
        self,
        pid: int,
        process_group: Optional[int] = None,
    ) -> Tuple[str, str]:
        return self._debugger.capture_process_stacks(pid, process_group)

    @staticmethod
    def _is_stop_signal(signal_num: Optional[int]) -> bool:
        if signal_num is None:
            return False
        return signal_num in _POSIX_STOP_SIGNALS

    def _capture_core_dump_stack(
        self,
        invocation: TestInvocation,
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        return self._debugger.capture_core_dump_stack(invocation, start_time, pid)

    def _collect_process_group_pids(self, pgid: int) -> List[int]:
        """Return all process IDs belonging to the provided process group."""

        return self.platform.collect_process_group_pids(pgid)

    def _collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Return all descendant process IDs for the provided root PID."""

        return self.platform.collect_descendant_pids(root_pid)

    def _describe_pids(self, pids: Iterable[int]) -> Dict[int, str]:
        """Return human-readable details for the provided process IDs."""

        return self.platform.describe_processes(pids)

    def _snapshot_process_table_via_ps(self) -> List[Tuple[int, int, int]]:
        """Return (pid, ppid, pgid) tuples using the portable ps command."""

        return self.platform.snapshot_process_table()


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
            print(f"[RUN INVOKE] {test_name} iter={i + 1}/{repetitions}", flush=True)
            result = self.run_test_once(invocation)
            results.append(result)

            if result.success:
                passed += 1
                print(f"{Color.GREEN}.{Color.RESET}", end='', flush=True)
            else:
                failed += 1
                print(f"{Color.RED}F{Color.RESET}", end='', flush=True)
                print(f"\n[ABORT] Stopping further repetitions due to failure/hang at iter {i + 1}", flush=True)
                break

            # Print newline every 50 tests for readability
            if (i + 1) % 50 == 0:
                print(f" [{i + 1}/{repetitions}]", end='', flush=True)
                if i + 1 < repetitions:
                    print("\n            ", end='', flush=True)

        print()  # Final newline

        if self.verbose:
            _, cleanup_messages = self.consume_core_cleanup_reports()
            for level, message in cleanup_messages:
                if level == 'warning':
                    print(f"  {Color.YELLOW}{message}{Color.RESET}")
                else:
                    print(f"  {message}")

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

        if _is_stack_capture_test(test_name):
            captured = sum(
                1 for r in results if "STACK CAPTURE: debugger/postmortem" in (r.error or "")
            )
            self_trace = sum(
                1 for r in results if "STACK CAPTURE: self-trace" in (r.error or "")
            )
            missing = sum(
                1 for r in results if "STACK CAPTURE: MISSING" in (r.error or "")
            )
            print(
                "  Stack capture probe: "
                f"captured={captured}, self-trace={self_trace}, missing={missing}"
            )

        # Print details of failures if verbose or if there are failures
        if failed > 0 and (self.verbose or failed <= 5):
            print(f"\n  {Color.YELLOW}Failure Details:{Color.RESET}")
            failure_count = 0
            for i, result in enumerate(results):
                if not result.success:
                    failure_count += 1

                    full_error_needed = (
                        self.verbose
                        or _is_stack_capture_test(test_name)
                        or '=== Captured stack traces ===' in result.error
                        or '=== Post-mortem stack trace ===' in result.error
                        or '[Stack capture unavailable' in result.error
                        or '[SINTRA_SELF_STACK_BEGIN]' in result.error
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
        description='Run Sintra tests with timeout support',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Test selection and iteration counts are controlled by tests/active_tests.txt'
    )
    parser.add_argument('--timeout', type=float, default=5.0,
                        help='Timeout per test run in seconds (default: 5)')
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

    # Parse active_tests.txt
    active_tests = parse_active_tests(script_dir)
    if not active_tests:
        print(f"{Color.RED}Error: No tests found in active_tests.txt{Color.RESET}")
        print(f"Please ensure tests/active_tests.txt exists and contains test entries")
        return 1

    compiler_metadata = collect_compiler_metadata(build_dir)

    print(f"Build directory: {build_dir}")
    print(f"Requested configuration root: {args.config}")
    if compiler_metadata and compiler_metadata.get("generator"):
        print(f"CMake generator: {compiler_metadata['generator']}")
    print(f"Timeout per test: {args.timeout}s")
    print(f"Active tests: {len(active_tests)} tests from active_tests.txt")
    print("=" * 70)

    if args.kill_stalled_processes:
        print(f"{Color.YELLOW}Warning: --kill_stalled_processes is deprecated; stalled tests are killed by default.{Color.RESET}")

    preserve_on_timeout = args.preserve_stalled_processes
    runner = TestRunner(build_dir, args.config, args.timeout, args.verbose, preserve_on_timeout)
    try:
        test_suites, active_tests = runner.find_test_suites(active_tests)

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
            print_configuration_build_details(compiler_metadata, config_name)
            print(f"{'=' * 80}")

            suite_start_time = time.time()

            # Adaptive soak test for this suite
            accumulated_results = {
                invocation.name: {'passed': 0, 'failed': 0, 'durations': [], 'failures': []}
                for invocation in tests
            }
            target_repetitions = {invocation.name: _lookup_test_weight(invocation.name, active_tests) for invocation in tests}
            remaining_repetitions = target_repetitions.copy()
            suite_all_passed = True
            batch_size = 1

            # Print test plan table for this suite
            print("\n  Test overview")

            # Calculate column widths
            id_col_width = 4
            name_col_width = max([len(_canonical_test_name(inv.name)) for inv in tests] + [4]) + 2
            reps_col_width = 12

            # Create format strings
            header_fmt = f"  {{:<{id_col_width}}} {{:<{name_col_width}}} {{:>{reps_col_width}}}"
            row_fmt = header_fmt
            table_width = id_col_width + name_col_width + reps_col_width + 4

            # Print table
            print("  " + "=" * table_width)
            print(header_fmt.format('ID', 'Name', 'Repetitions'))
            print("  " + "=" * table_width)

            for idx, invocation in enumerate(tests, start=1):
                canonical_name = _canonical_test_name(invocation.name)
                reps = target_repetitions[invocation.name]
                print(row_fmt.format(f"{idx:02d}", canonical_name, reps))

            print("  " + "=" * table_width)

            # Create header for round display
            header = " ".join(f"{index + 1:02d}" for index, _ in enumerate(tests))

            while True:
                remaining_counts = [count for count in remaining_repetitions.values() if count > 0]
                if not remaining_counts:
                    break

                max_remaining = max(remaining_counts)
                reps_in_this_round = min(batch_size, max_remaining)
                print(f"\n{Color.BLUE}--- Round: {reps_in_this_round} repetition(s) ---{Color.RESET}")
                print(header)

                lingering = find_lingering_processes(("sintra_",))
                if lingering:
                    details = _describe_processes(lingering)
                    print(
                        f"  {Color.YELLOW}Warning: Detected lingering sintra processes before starting the round: {details}{Color.RESET}"
                    )

                for i in range(reps_in_this_round):
                    row_segments = ["  "] * len(tests)
                    for index, invocation in enumerate(tests):
                        test_name = invocation.name
                        if remaining_repetitions[test_name] <= 0:
                            continue

                        row_start = "."
                        result = runner.run_test_once(invocation)

                        accumulated_results[test_name]['durations'].append(result.duration)

                        result_bucket = accumulated_results[test_name]

                        if result.success:
                            result_bucket['passed'] += 1
                            row_end = "."
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

                            row_end = "F"

                        remaining_repetitions[test_name] -= 1

                        row_segments[index] = f"{row_start}{row_end}"

                    print(" ".join(row_segments))
                    if not suite_all_passed:
                        break

                round_elapsed = time.time() - suite_start_time
                print(
                    f"    {Color.BLUE}Round complete - total elapsed: {format_duration(round_elapsed)}{Color.RESET}"
                )

                available_memory = runner.platform.available_memory_bytes()
                disk_space = available_disk_bytes(runner.build_dir)
                print(
                    f"    Diagnostics: available memory={format_size(available_memory)}, "
                    f"free disk={format_size(disk_space)}"
                )

                if runner.verbose:
                    _, cleanup_messages = runner.consume_core_cleanup_reports()
                    for level, message in cleanup_messages:
                        if level == 'warning':
                            print(f"    {Color.YELLOW}{message}{Color.RESET}")
                        else:
                            print(f"    {message}")

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

            # Add spacing between suites if not the last one
            if config_idx < total_configs:
                print()
                print()

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
    finally:
        runner.cleanup()

if __name__ == '__main__':
    sys.exit(main())
