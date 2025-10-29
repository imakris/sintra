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
    --config CONFIG                 Build configuration Debug/Release (default: Debug)
    --verbose                       Show detailed output for each test run
    --preserve-stalled-processes    Keep stalled processes running for debugging (default: terminate)
"""

import argparse
import fnmatch
import importlib
import importlib.util
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, IO, Iterable, List, Optional, Sequence, Set, Tuple


_PSUTIL = None
if importlib.util.find_spec("psutil") is not None:
    _PSUTIL = importlib.import_module("psutil")


PRESERVE_CORES_ENV = "SINTRA_PRESERVE_CORES"

# macOS emits Mach-O core files that snapshot every virtual memory region of the
# crashing process. Two platform effects make Sintra dumps look enormous even
# when very little physical memory is dirtied:
#
#   • The dynamic loader reserves a 4 GiB ``__PAGEZERO`` segment on every
#     process. That reservation is always recorded in the core image even though
#     it contains no data.
#   • Every request/reply ring that ``Managed_process`` touches is "double
#     mapped" by ``Ring_data::attach``: we reserve a 2× span and map the 2 MiB
#     data file twice so wrap-around reads stay linear. Each active channel
#     therefore contributes roughly 4 MiB of virtual address space. During the
#     recovery test the coordinator plus the watchdog/crasher pair keep dozens of
#     these channels open concurrently (outgoing rings for the local process plus
#     readers for every remote process slot), which adds roughly another 250 MiB
#     of reservations.
#
# GitHub Actions used to report the logical size of that 4 GiB + ~250 MiB address
# space (~4.24 GiB) as soon as a Mach-O core was written even though the rings
# are sparse files on APFS. ``MADV_DONTDUMP`` handles this automatically on
# Linux. macOS does not expose the flag, so ``recovery_test`` disables core
# dumps immediately before its intentional ``std::abort()`` to keep runners from
# filling their disks with Mach-O artifacts.


def _format_size(num_bytes: Optional[int]) -> str:
    """Return a human-friendly representation of ``num_bytes``."""

    if num_bytes is None:
        return "unknown"

    if num_bytes < 0:
        return "unknown"

    units = ["B", "KB", "MB", "GB", "TB", "PB"]
    value = float(num_bytes)
    for unit in units:
        if value < 1024.0:
            return f"{value:.2f} {unit}"
        value /= 1024.0
    return f"{value:.2f} EB"


def _available_memory_bytes() -> Optional[int]:
    """Best-effort determination of available physical memory."""

    if _PSUTIL is not None:
        try:
            return int(_PSUTIL.virtual_memory().available)
        except Exception:
            pass

    try:
        page_size = os.sysconf("SC_PAGE_SIZE")
        avail_pages = os.sysconf("SC_AVPHYS_PAGES")
    except (AttributeError, OSError, ValueError):
        page_size = None
        avail_pages = None

    if isinstance(page_size, int) and isinstance(avail_pages, int):
        return page_size * avail_pages

    if sys.platform == "darwin":
        try:
            vm_stat = subprocess.run(
                ["vm_stat"],
                check=True,
                capture_output=True,
                text=True,
            )
        except Exception:
            return None

        page_size_bytes = 4096
        for line in vm_stat.stdout.splitlines():
            if line.startswith("page size of"):
                parts = line.split()
                try:
                    page_size_bytes = int(parts[3])
                except (IndexError, ValueError):
                    page_size_bytes = 4096
                break

        free_pages = 0
        inactive_pages = 0
        speculative_pages = 0

        for line in vm_stat.stdout.splitlines():
            if line.startswith("Pages free"):
                free_pages = int(line.split(":")[1].strip().strip("."))
            elif line.startswith("Pages inactive"):
                inactive_pages = int(line.split(":")[1].strip().strip("."))
            elif line.startswith("Pages speculative"):
                speculative_pages = int(line.split(":")[1].strip().strip("."))

        return page_size_bytes * (free_pages + inactive_pages + speculative_pages)

    return None


def _available_disk_bytes(path: Path) -> Optional[int]:
    """Return free disk space for ``path``."""

    try:
        usage = shutil.disk_usage(path)
    except Exception:
        return None
    return usage.free


def _env_flag(name: str) -> bool:
    """Return True if the specified environment variable is truthy."""

    value = os.environ.get(name)
    if value is None:
        return False

    normalized = value.strip().lower()
    if not normalized:
        return False

    return normalized not in {"0", "false", "no", "off"}


def _find_lingering_processes(prefixes: Sequence[str]) -> List[Tuple[int, str]]:
    """Return processes whose names start with one of ``prefixes``."""

    normalized_prefixes = tuple(prefixes)
    matches: List[Tuple[int, str]] = []

    if _PSUTIL is not None:
        try:
            for proc in _PSUTIL.process_iter(["name"]):
                name = proc.info.get("name") or ""
                if name.startswith(normalized_prefixes):
                    matches.append((proc.pid, name))
        except Exception:
            pass
        if matches:
            return matches

    try:
        ps_output = subprocess.run(
            ["ps", "-axo", "pid=,comm="],
            check=True,
            capture_output=True,
            text=True,
        )
    except Exception:
        return matches

    for line in ps_output.stdout.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        try:
            pid = int(parts[0])
        except ValueError:
            continue
        name = parts[1]
        if name.startswith(normalized_prefixes):
            matches.append((pid, name))

    return matches


def _describe_processes(processes: Iterable[Tuple[int, str]]) -> str:
    """Return a compact string description of ``processes``."""

    formatted = [f"{name} (pid={pid})" for pid, name in processes]
    return ", ".join(formatted) if formatted else ""

class Color:
    """ANSI color codes for terminal output"""
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


# Tests that should receive additional repetition weight. Each value represents
# the minimum number of times that the corresponding test should run when the
# global ``--repetitions`` argument is set to 1. Larger ``--repetitions`` values
# can still increase the total runs for a test, but the override no longer
# multiplies the global value directly (which previously caused runaway runtimes
# when soak runs used high repetition counts).
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

# Tests that need extended timeouts beyond the global ``--timeout`` argument.
# Values represent the minimum timeout (in seconds) that should be enforced for
# the corresponding test invocation.
TEST_TIMEOUT_OVERRIDES = {
    "recovery_test_debug": 120.0,
    "recovery_test_release": 120.0,
}

# Configure the maximum amount of wall time the runner spends attaching live
# debuggers before declaring stack capture unavailable. Users can extend this by
# setting ``SINTRA_LIVE_STACK_ATTACH_TIMEOUT`` (in seconds) when particularly
# heavy stress suites need more time.
LIVE_STACK_ATTACH_TIMEOUT_ENV = 'SINTRA_LIVE_STACK_ATTACH_TIMEOUT'
DEFAULT_LIVE_STACK_ATTACH_TIMEOUT = 30.0
DEFAULT_LLDB_LIVE_STACK_ATTACH_TIMEOUT = 90.0

WINDOWS_DEBUGGER_CACHE_ENV = 'SINTRA_WINDOWS_DEBUGGER_CACHE'
WINSDK_INSTALLER_URL_ENV = 'SINTRA_WINSDK_INSTALLER_URL'
WINSDK_FEATURE_ENV = 'SINTRA_WINSDK_FEATURE'
WINSDK_DEBUGGER_MSI_ENV = 'SINTRA_WINSDK_DEBUGGER_MSI'
WINSDK_INSTALLER_URL = os.environ.get(
    WINSDK_INSTALLER_URL_ENV,
    'https://download.microsoft.com/download/7/9/6/7962e9ce-cd69-4574-978c-1202654bd729/windowssdk/winsdksetup.exe',
)
WINSDK_FEATURE_ID = os.environ.get(WINSDK_FEATURE_ENV, 'OptionId.WindowsDesktopDebuggers')
WINSDK_DEBUGGER_MSI_NAME = os.environ.get(
    WINSDK_DEBUGGER_MSI_ENV,
    'X64 Debuggers And Tools-x64_en-us.msi',
)


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


def _lookup_test_timeout(name: str, default: float) -> float:
    """Return the timeout for the provided test invocation."""

    canonical = _canonical_test_name(name)
    override = TEST_TIMEOUT_OVERRIDES.get(canonical)
    if override is None:
        return default
    return max(default, override)


def _calculate_target_repetitions(base_repetitions: int, weight: int) -> int:
    """Return the total repetitions to run for a test.

    ``weight`` expresses the desired run count when ``base_repetitions`` is 1.
    Increasing ``base_repetitions`` beyond 1 no longer multiplies the weight,
    preventing exponential growth in soak runs with large weight overrides.
    ``base_repetitions`` can still override the weight when set higher.
    """

    base = max(base_repetitions, 0)
    if base == 0:
        return 0

    if weight <= 1:
        return base

    if base == 1:
        return weight

    return max(base, weight)

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
        sanitized_config = ''.join(c.lower() if c.isalnum() else '_' for c in config)
        timestamp_ms = int(time.time() * 1000)
        scratch_dir_name = f".sintra-test-scratch-{sanitized_config}-{timestamp_ms}-{os.getpid()}"
        self._scratch_base = (self.build_dir / scratch_dir_name).resolve()
        self._scratch_base.mkdir(parents=True, exist_ok=True)
        self._scratch_lock = threading.Lock()
        self._scratch_counter = 0
        self._ipc_rings_cache: Dict[Path, List[Tuple[str, str]]] = {}
        self._debugger_cache: Dict[str, Tuple[Optional[str], str]] = {}
        self._downloaded_windows_debugger_root: Optional[Path] = None
        self._stack_capture_history: Dict[str, Set[str]] = defaultdict(set)
        self._stack_capture_history_lock = threading.Lock()
        self._sudo_capability: Optional[bool] = None
        self._windows_crash_dump_dir: Optional[Path] = None
        self._preserve_core_dumps = _env_flag(PRESERVE_CORES_ENV)
        self._core_cleanup_lock = threading.Lock()
        self._core_cleanup_messages: List[Tuple[str, str]] = []
        self._core_cleanup_bytes_freed = 0
        self._scratch_cleanup_lock = threading.Lock()
        self._scratch_cleanup_dirs_removed = 0
        self._scratch_cleanup_bytes_freed = 0

        # Determine test directory - check both with and without config subdirectory
        test_dir_with_config = build_dir / 'tests' / config
        test_dir_simple = build_dir / 'tests'

        if test_dir_with_config.exists():
            self.test_dir = test_dir_with_config
        else:
            self.test_dir = test_dir_simple

        # Kill any existing sintra processes for a clean start
        self._kill_all_sintra_processes()

        if sys.platform == 'win32':
            self._prepare_windows_debuggers()
            dump_error = self._ensure_windows_local_dumps()
            if dump_error:
                print(
                    f"{Color.YELLOW}Warning: {dump_error}. "
                    f"Crash dumps may be unavailable.{Color.RESET}"
                )
            jit_error = self._configure_windows_jit_debugging()
            if jit_error:
                print(
                    f"{Color.YELLOW}Warning: {jit_error}. "
                    f"JIT prompts may still block crash dumps.{Color.RESET}"
                )

    def _allocate_scratch_directory(self, invocation: TestInvocation) -> Path:
        """Create a per-invocation scratch directory for test artifacts."""

        safe_name = ''.join(c if c.isalnum() or c in ('-', '_') else '_' for c in invocation.name)
        with self._scratch_lock:
            index = self._scratch_counter
            self._scratch_counter += 1

        directory = self._scratch_base / f"{safe_name}_{index}"
        directory.mkdir(parents=True, exist_ok=True)
        return directory

    def _build_test_environment(self, scratch_dir: Path) -> Dict[str, str]:
        """Return the environment variables for a test invocation."""

        env = os.environ.copy()
        env['SINTRA_TEST_ROOT'] = str(scratch_dir)
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

        if sys.platform == "darwin":
            candidates.add(Path("/cores"))
            candidates.add(Path.home() / "Library" / "Logs" / "DiagnosticReports")
        elif sys.platform.startswith("linux"):
            candidates.add(Path("/var/lib/systemd/coredump"))

        return [path for path in candidates if path]

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
                f"{len(new_dumps)} file(s) totalling {_format_size(total_size)}"
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
                f"{len(removed)} file(s) totalling {_format_size(freed_bytes)}"
                f" ({', '.join(removed)})"
            )
            level = "warning" if result_success else "info"
            self._record_core_cleanup(level, message, freed_bytes=freed_bytes)
        else:
            # Nothing removed; still report total detected to aid diagnostics.
            message = (
                f"{message_prefix}: detected "
                f"{len(new_dumps)} file(s) totalling {_format_size(total_size)}"
            )
            level = "warning" if result_success else "info"
            self._record_core_cleanup(level, message)

        for error in errors:
            self._record_core_cleanup("warning", error)

    def cleanup(self) -> None:
        """Remove the root scratch directory for this runner."""

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
                        f"{scratch_dirs} {noun} totalling {_format_size(scratch_bytes)}"
                    ),
                )
            )

        return total_freed, messages

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
            'debug',
            'release'
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
        timeout = _lookup_test_timeout(invocation.name, self.timeout)
        scratch_dir = self._allocate_scratch_directory(invocation)
        process = None
        cleanup_scratch_dir = True
        core_snapshot = self._snapshot_core_dumps(invocation)
        start_time = time.time()
        result_success: Optional[bool] = None
        try:
            popen_env = self._build_test_environment(scratch_dir)
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
            popen_kwargs['env'] = popen_env

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
            threads: List[Tuple[threading.Thread, IO[str]]] = []
            process_group_id: Optional[int] = None

            def shutdown_reader_threads() -> None:
                """Ensure log reader threads terminate to avoid leaking resources."""
                join_step = 0.2
                max_join_time = 5.0

                for thread, stream in threads:
                    join_deadline = time.monotonic() + max_join_time
                    while thread.is_alive():
                        remaining = join_deadline - time.monotonic()
                        if remaining <= 0:
                            break
                        thread.join(timeout=min(join_step, remaining))

                    if thread.is_alive():
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

            def attempt_live_capture(trigger_line: str) -> None:
                nonlocal live_stack_traces, live_stack_error, capture_pause_total, capture_active_start
                if not trigger_line:
                    return
                if not self._line_indicates_failure(trigger_line):
                    return
                if not self._should_attempt_stack_capture(invocation, 'live_failure'):
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
                    traces, error = self._capture_process_stacks(
                        process.pid,
                        process_group_id,
                    )
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

            if process.stdout:
                stdout_thread = threading.Thread(
                    target=monitor_stream,
                    args=(process.stdout, stdout_lines),
                    daemon=True,
                )
                stdout_thread.start()
                threads.append((stdout_thread, process.stdout))

            if process.stderr:
                stderr_thread = threading.Thread(
                    target=monitor_stream,
                    args=(process.stderr, stderr_lines),
                    daemon=True,
                )
                stderr_thread.start()
                threads.append((stderr_thread, process.stderr))

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
                duration = time.time() - start_time

                shutdown_reader_threads()

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

                        if sys.platform == 'win32':
                            if self._should_attempt_stack_capture(invocation, 'postmortem'):
                                (
                                    postmortem_stack_traces,
                                    postmortem_stack_error,
                                ) = self._capture_windows_crash_dump(invocation, start_time, process.pid)
                        else:
                            if self._should_attempt_stack_capture(invocation, 'postmortem'):
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
            if cleanup_scratch_dir:
                self._cleanup_scratch_directory(scratch_dir)
            self._cleanup_new_core_dumps(invocation, core_snapshot, start_time, result_success)

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

    def _should_attempt_stack_capture(self, invocation: TestInvocation, reason: str) -> bool:
        """Return True if stack capture for the invocation/reason has not run yet."""

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

        if sys.platform == 'win32':
            # Windows crash codes are typically large unsigned values such as 0xC0000005.
            return returncode < 0 or returncode >= 0xC0000000

        # POSIX: negative return codes indicate termination by signal.
        # Codes > 128 are also commonly used to report signal-based exits.
        return returncode < 0 or returncode > 128

    def _capture_process_stacks(
        self,
        pid: int,
        process_group: Optional[int] = None,
    ) -> Tuple[str, str]:
        """Attempt to capture stack traces for the test process and all of its helpers."""

        if sys.platform == 'win32':
            return self._capture_process_stacks_windows(pid)

        debugger_name, debugger_command, debugger_error = self._resolve_unix_debugger()
        if not debugger_command:
            return "", debugger_error

        import signal
        pgid: Optional[int] = None
        if process_group is not None:
            pgid = process_group
        else:
            try:
                pgid = os.getpgid(pid)
            except ProcessLookupError:
                return "", "process exited before stack capture"

        target_pids = set()
        target_pids.add(pid)
        if pgid is not None:
            target_pids.update(self._collect_process_group_pids(pgid))
        target_pids.update(self._collect_descendant_pids(pid))

        if not target_pids:
            target_pids = {pid}

        def _pid_exists(target_pid: int) -> bool:
            if target_pid <= 0:
                return False
            if sys.platform == 'win32':
                return True
            try:
                os.kill(target_pid, 0)
            except ProcessLookupError:
                return False
            except PermissionError:
                return True
            except OSError:
                return False
            return True

        paused_pids = []
        paused_groups: List[int] = []
        stack_outputs = []
        capture_errors = []

        # Pause each discovered process individually so we can capture consistent stacks,
        # even if helpers spawned new process groups.
        try:
            import signal
        except ImportError:
            signal = None

        debugger_is_macos_lldb = debugger_name == 'lldb' and sys.platform == 'darwin'
        # Pausing processes helps capture coherent stacks, but LLDB on macOS fails
        # to attach to tasks that are already SIGSTOPed. Skip the pause in that case
        # and let LLDB suspend threads itself.
        should_pause = signal is not None and not debugger_is_macos_lldb

        if should_pause:
            if pgid is not None and pgid > 0:
                try:
                    if os.getpgrp() != pgid:
                        os.killpg(pgid, signal.SIGSTOP)
                        paused_groups.append(pgid)
                except (ProcessLookupError, PermissionError, OSError):
                    pass

            for target_pid in sorted(target_pids):
                if target_pid == os.getpid():
                    continue
                if not _pid_exists(target_pid):
                    continue
                try:
                    os.kill(target_pid, signal.SIGSTOP)
                    paused_pids.append(target_pid)
                except (ProcessLookupError, PermissionError):
                    continue

        if debugger_is_macos_lldb:
            # On macOS LLDB fails to attach to processes that are already stopped,
            # so we let the debugger suspend threads itself. The helper processes
            # for a test typically have higher PIDs than the main test binary and
            # may exit quickly once the parent crashes. Capture those helpers
            # first to maximise the chance of getting a stack trace before they
            # disappear.
            ordered_target_pids = sorted(target_pids, reverse=True)
        else:
            ordered_target_pids = sorted(target_pids)

        env_timeout = os.environ.get(LIVE_STACK_ATTACH_TIMEOUT_ENV)
        attach_timeout = 0.0
        if env_timeout:
            try:
                attach_timeout = float(env_timeout)
            except ValueError:
                attach_timeout = 0.0

        if attach_timeout <= 0.0:
            if debugger_is_macos_lldb:
                attach_timeout = DEFAULT_LLDB_LIVE_STACK_ATTACH_TIMEOUT
            else:
                attach_timeout = DEFAULT_LIVE_STACK_ATTACH_TIMEOUT

        per_pid_timeout = min(30.0, attach_timeout)
        total_budget = attach_timeout
        capture_deadline = time.monotonic() + total_budget
        attach_timeout_seconds = attach_timeout

        attach_timeout_hint_needed = False

        for target_pid in ordered_target_pids:
            if target_pid == os.getpid():
                continue
            if not _pid_exists(target_pid):
                capture_errors.append(
                    f"PID {target_pid}: process exited before stack capture"
                )
                continue

            remaining = capture_deadline - time.monotonic()
            if remaining <= 0:
                capture_errors.append(
                    f"PID {target_pid}: skipped stack capture (overall debugger timeout exceeded)"
                )
                attach_timeout_hint_needed = True
                break
            debugger_timeout = min(
                remaining,
                max(5.0, min(per_pid_timeout, remaining)),
            )

            debugger_output = ""
            debugger_error = ""
            debugger_success = False

            max_attempts = 3 if debugger_is_macos_lldb else 1
            base_command = self._build_unix_live_debugger_command(
                debugger_name,
                debugger_command,
                target_pid,
            )
            attempt = 0
            use_sudo = False
            while attempt < max_attempts:
                command = (
                    self._wrap_command_with_sudo(base_command)
                    if use_sudo
                    else base_command
                )
                try:
                    result = subprocess.run(
                        command,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        timeout=debugger_timeout,
                    )
                except subprocess.TimeoutExpired:
                    debugger_error = (
                        f"{debugger_name} timed out after {debugger_timeout:.1f}s while attaching"
                    )
                    attach_timeout_hint_needed = True
                    break
                except subprocess.SubprocessError as exc:
                    debugger_error = f"{debugger_name} failed ({exc})"
                    break

                debugger_output = result.stdout.strip()
                if not debugger_output and result.stderr:
                    debugger_output = result.stderr.strip()

                if result.returncode == 0:
                    debugger_success = True
                    break

                debugger_error = (
                    f"{debugger_name} exited with code {result.returncode}: {result.stderr.strip()}"
                )

                if (
                    not use_sudo
                    and self._should_retry_with_sudo(result.stderr, result.stdout)
                ):
                    use_sudo = True
                    continue

                if not debugger_is_macos_lldb:
                    break

                if not self._process_exists(target_pid):
                    break

                lowered_error = result.stderr.strip().lower()
                if 'no such process' not in lowered_error and 'does not exist' not in lowered_error:
                    break

                time.sleep(0.5)

                attempt += 1

            if debugger_success:
                if debugger_output:
                    stack_outputs.append(f"PID {target_pid}\n{debugger_output}")
                continue

            fallback_output = ""
            fallback_error = debugger_error

            if debugger_is_macos_lldb and self._process_exists(target_pid):
                fallback_output, fallback_error = self._capture_macos_sample_stack(
                    target_pid,
                    debugger_timeout,
                )

            if fallback_output:
                stack_outputs.append(f"PID {target_pid}\n{fallback_output}")
                continue

            if fallback_error:
                capture_errors.append(f"PID {target_pid}: {fallback_error}")

        # Allow processes to continue so gdb can detach cleanly before killing
        if should_pause:
            for target_pid in paused_pids:
                try:
                    os.kill(target_pid, signal.SIGCONT)
                except (ProcessLookupError, PermissionError):
                    continue

            for group_id in paused_groups:
                try:
                    if os.getpgrp() != group_id:
                        os.killpg(group_id, signal.SIGCONT)
                except (ProcessLookupError, PermissionError, OSError):
                    continue

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if attach_timeout_hint_needed:
            capture_errors.append(
                (
                    "Live stack capture timed out after "
                    f"{attach_timeout_seconds:.1f}s; increase "
                    f"{LIVE_STACK_ATTACH_TIMEOUT_ENV} to allow more debugger time"
                )
            )

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
            if sys.platform == 'darwin':
                crash_output, crash_error = self._capture_macos_crash_report_stack(
                    invocation,
                    start_time,
                    pid,
                )
                if crash_output or crash_error:
                    return crash_output, crash_error
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

        if sys.platform == 'darwin':
            dsym_error = self._ensure_macos_dsym(invocation.path)
            if dsym_error:
                capture_errors.append(f"{invocation.path.name}: {dsym_error}")

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

    def _capture_macos_crash_report_stack(
        self,
        invocation: TestInvocation,
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        """Attempt to extract stack traces from recent macOS crash reports."""

        crash_dirs = [
            Path.home() / 'Library' / 'Logs' / 'DiagnosticReports',
            Path('/Library/Logs/DiagnosticReports'),
        ]

        process_stem = invocation.path.stem
        stem_lower = process_stem.lower()
        name_lower = invocation.path.name.lower()
        candidates: List[Tuple[float, Path]] = []

        for directory in crash_dirs:
            try:
                entries = list(directory.iterdir())
            except OSError:
                continue

            for entry in entries:
                if not entry.is_file():
                    continue

                entry_lower = entry.name.lower()
                if stem_lower not in entry_lower and name_lower not in entry_lower:
                    continue

                if entry.suffix.lower() not in {'.ips', '.crash'}:
                    continue

                try:
                    stat_info = entry.stat()
                except OSError:
                    continue

                if stat_info.st_mtime + 0.001 < start_time:
                    continue

                candidates.append((stat_info.st_mtime, entry))

        if not candidates:
            return "", "no recent macOS crash report found"

        candidates.sort(key=lambda item: item[0], reverse=True)

        parse_errors: List[str] = []

        for _, crash_path in candidates:
            if crash_path.suffix.lower() == '.ips':
                stack, error = self._parse_macos_ips_report(crash_path, pid)
            else:
                stack, error = self._parse_macos_crash_report(crash_path, pid)

            if stack:
                return f"crash report: {crash_path}\n{stack}", ""
            if error:
                parse_errors.append(f"{crash_path.name}: {error}")

        if parse_errors:
            return "", "; ".join(parse_errors)

        return "", "macOS crash report did not contain stack information"

    def _parse_macos_crash_report(self, path: Path, pid: int) -> Tuple[str, str]:
        """Parse legacy text-based crash reports produced by Crash Reporter."""

        try:
            content = path.read_text(errors='replace')
        except OSError as exc:
            return "", f"failed to read crash report: {exc}"

        lines = content.splitlines()
        crash_section: List[str] = []
        collecting = False

        for line in lines:
            stripped = line.strip()
            if stripped.startswith('Thread') and 'Crashed' in stripped:
                crash_section = [stripped]
                collecting = True
                continue

            if not collecting:
                continue

            if not stripped:
                break

            crash_section.append(stripped)

        if crash_section:
            return '\n'.join(crash_section), ""

        if str(pid) in content:
            # Provide the tail of the report as a best-effort diagnostic.
            tail = '\n'.join(lines[-20:])
            return tail, ""

        return "", "crash report did not contain a crashed thread section"

    def _parse_macos_ips_report(self, path: Path, pid: int) -> Tuple[str, str]:
        """Parse modern structured .ips crash reports to extract stack traces."""

        try:
            content = path.read_text()
        except OSError as exc:
            return "", f"failed to read crash report: {exc}"

        try:
            data = json.loads(content)
        except json.JSONDecodeError as exc:
            return "", f"invalid JSON in crash report: {exc}"

        roots = []
        if isinstance(data, dict):
            if isinstance(data.get('roots'), list):
                roots = [item for item in data['roots'] if isinstance(item, dict)]
            else:
                roots = [data]

        if not roots:
            return "", "unexpected .ips crash report structure"

        errors: List[str] = []

        for root in roots:
            threads = root.get('threads')
            if not isinstance(threads, list):
                continue

            triggered_thread = None
            for thread in threads:
                if not isinstance(thread, dict):
                    continue
                if thread.get('triggered'):
                    triggered_thread = thread
                    break
            if triggered_thread is None:
                triggered_thread = next((t for t in threads if isinstance(t, dict)), None)
            if triggered_thread is None:
                continue

            frames = triggered_thread.get('frames')
            if not isinstance(frames, list) or not frames:
                continue

            image_map: Dict[int, str] = {}
            for image in root.get('binaryImages', []):
                if not isinstance(image, dict):
                    continue
                index = image.get('imageIndex')
                if index is None:
                    index = image.get('index')
                if index is None:
                    continue
                name = image.get('name') or image.get('path') or image.get('imageName')
                if not name:
                    name = image.get('uuid') or 'unknown image'
                image_map[int(index)] = str(name)

            formatted_frames: List[str] = []
            for frame_index, frame in enumerate(frames):
                if not isinstance(frame, dict):
                    continue
                symbol = frame.get('symbol') or frame.get('function') or '<unknown>'
                symbol_location = frame.get('symbolLocation')
                if symbol_location is None:
                    symbol_location = frame.get('offset')
                image_index = frame.get('imageIndex')
                if image_index is None:
                    image_index = frame.get('binaryImageIndex')
                image_name = image_map.get(int(image_index)) if image_index is not None else None
                image_offset = frame.get('imageOffset')

                parts = [f"{frame_index:02d} {symbol}"]
                if symbol_location is not None:
                    parts.append(f"+ {symbol_location}")
                elif image_offset is not None:
                    parts.append(f"@ {image_offset}")
                if image_name:
                    parts.append(f"({image_name})")

                formatted_frames.append(' '.join(str(part) for part in parts if part))

            if formatted_frames:
                thread_name = triggered_thread.get('name') or triggered_thread.get('id')
                header = f"Thread: {thread_name}" if thread_name else "Thread"
                return f"{header}\n" + '\n'.join(formatted_frames), ""

            errors.append('no frames in triggered thread')

        if errors:
            return "", '; '.join(errors)

        return "", "no triggered thread found in .ips crash report"

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

    def _process_exists(self, pid: int) -> bool:
        """Best-effort check whether a PID still refers to a running process."""

        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False

    def _capture_macos_sample_stack(self, pid: int, timeout: float) -> Tuple[str, str]:
        """Attempt to capture stack traces using the macOS sample utility."""

        sample_path = shutil.which('sample')
        if not sample_path:
            return "", "sample utility not available"

        duration = max(1, min(10, int(timeout)))
        command = [
            sample_path,
            str(pid),
            str(duration),
            '1',
            '-mayDie',
        ]

        sudo_supported = self._supports_passwordless_sudo()
        for use_sudo in (False, True) if sudo_supported else (False,):
            actual_command = (
                self._wrap_command_with_sudo(command) if use_sudo else command
            )

            try:
                result = subprocess.run(
                    actual_command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=duration + 5,
                )
            except subprocess.SubprocessError as exc:
                error = f"sample failed ({exc})"
                if not use_sudo and sudo_supported:
                    continue
                return "", error

            if result.returncode == 0:
                output = result.stdout.strip()
                if not output:
                    return "", "sample produced no output"
                return output, ""

            detail = result.stderr.strip() or result.stdout.strip()
            error_message = (
                f"sample exited with code {result.returncode}: {detail}"
                if detail
                else f"sample exited with code {result.returncode}"
            )

            if use_sudo or not self._should_retry_with_sudo(result.stderr, result.stdout):
                return "", error_message

        return "", "sample exited without producing output"

    def _supports_passwordless_sudo(self) -> bool:
        if self._sudo_capability is not None:
            return self._sudo_capability

        if sys.platform != 'darwin':
            self._sudo_capability = False
            return False

        if os.geteuid() == 0:
            self._sudo_capability = False
            return False

        sudo_path = shutil.which('sudo')
        if not sudo_path:
            self._sudo_capability = False
            return False

        try:
            result = subprocess.run(
                [sudo_path, '-n', 'true'],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
            )
        except subprocess.SubprocessError:
            self._sudo_capability = False
            return False

        self._sudo_capability = result.returncode == 0
        return self._sudo_capability

    def _wrap_command_with_sudo(self, command: List[str]) -> List[str]:
        if not self._supports_passwordless_sudo():
            return command
        return ['sudo', '-n', *command]

    def _should_retry_with_sudo(self, stderr: str, stdout: str) -> bool:
        if not self._supports_passwordless_sudo():
            return False

        combined = f"{stderr}\n{stdout}".lower()
        keywords = [
            'operation not permitted',
            'not permitted',
            'permission denied',
            'access denied',
            'try running with `sudo`',
            "try running with 'sudo'",
            'requires root',
            'must be run as root',
            'security policy prevents',
            'code signature may be invalid',
            'cannot examine process',
            'failed to get task for process',
        ]

        return any(keyword in combined for keyword in keywords)

    def _ensure_macos_dsym(self, executable: Path) -> Optional[str]:
        """Ensure a dSYM bundle exists for the provided executable on macOS."""

        dsym_dir = executable.with_name(f"{executable.name}.dSYM")
        if dsym_dir.exists():
            return None

        dsym_command: Optional[List[str]] = None

        dsymutil_path = shutil.which('dsymutil')
        if dsymutil_path:
            dsym_command = [dsymutil_path, str(executable)]
        else:
            xcrun_path = shutil.which('xcrun')
            if xcrun_path:
                dsym_command = [xcrun_path, 'dsymutil', str(executable)]

        if dsym_command is None:
            return 'dsymutil not available (install Xcode Command Line Tools)'

        try:
            result = subprocess.run(
                dsym_command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=120,
            )
        except subprocess.SubprocessError as exc:
            return f'dsymutil invocation failed ({exc})'

        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            if detail:
                return f'dsymutil exited with code {result.returncode}: {detail}'
            return f'dsymutil exited with code {result.returncode}'

        return None

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
                '--quiet',
                '--nx',
                '-p', str(pid),
                '-ex', 'set confirm off',
                '-ex', 'set pagination off',
                '-ex', 'thread apply all bt full',
                '-ex', 'detach',
                '-ex', 'quit',
            ]

        # Fallback to lldb
        return [
            *debugger_command,
            '--batch',
            '--no-lldbinit',
            '-p', str(pid),
            '-o', 'thread backtrace all -c 256 -f',
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
                '--quiet',
                '--nx',
                '-ex', 'set confirm off',
                '-ex', 'set pagination off',
                '-ex', 'thread apply all bt full',
                '-ex', 'quit',
                str(invocation.path),
                str(core_path),
            ]

        # Fallback to lldb
        quoted_executable = shlex.quote(str(invocation.path))
        quoted_core = shlex.quote(str(core_path))

        return [
            *debugger_command,
            '--batch',
            '--no-lldbinit',
            '-o', f'target create --core {quoted_core} {quoted_executable}',
            '-o', 'thread backtrace all -c 256 -f',
            '-o', 'quit',
        ]

    def _locate_windows_debugger(self, executable: str) -> Tuple[Optional[str], str]:
        """Locate or install the requested Windows debugger executable."""

        if sys.platform != 'win32':
            return None, f"{executable} not available on this platform"

        cache_key = executable.lower()
        if cache_key in self._debugger_cache:
            return self._debugger_cache[cache_key]

        debugger_root, prepare_error = self._ensure_downloaded_windows_debugger_root()
        if not debugger_root:
            error = prepare_error or f"failed to prepare debugger payload for {executable}"
            result = (None, error)
            self._debugger_cache[cache_key] = result
            return result

        candidates = [executable]
        if not executable.lower().endswith('.exe'):
            candidates.append(f"{executable}.exe")

        located = self._find_downloaded_debugger_executable(debugger_root, candidates)
        if located:
            result = (str(located), '')
            self._debugger_cache[cache_key] = result
            return result

        error = f"{executable} not found in downloaded debugger cache ({debugger_root})"
        result = (None, error)
        self._debugger_cache[cache_key] = result
        return result

    def _ensure_downloaded_windows_debugger_root(self) -> Tuple[Optional[Path], str]:
        """Ensure Debugging Tools for Windows are cached and return their root."""

        if self._downloaded_windows_debugger_root:
            return self._downloaded_windows_debugger_root, ''

        cache_dir = self._get_windows_debugger_cache_dir()
        try:
            cache_dir.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return None, f"failed to create debugger cache directory {cache_dir}: {exc}"

        install_root = cache_dir / 'winsdk_debuggers'
        debugger_root = install_root / 'Windows Kits' / '10' / 'Debuggers'
        sentinel = debugger_root / 'x64' / 'cdb.exe'
        try:
            if sentinel.exists():
                self._downloaded_windows_debugger_root = debugger_root
                return debugger_root, ''
        except OSError:
            pass

        layout_dir = cache_dir / 'winsdk_layout'
        msi_path = layout_dir / 'Installers' / WINSDK_DEBUGGER_MSI_NAME

        if not msi_path.exists():
            layout_error = self._ensure_winsdk_layout(layout_dir)
            if layout_error:
                return None, layout_error
            if not msi_path.exists():
                return None, f"expected debugger MSI {WINSDK_DEBUGGER_MSI_NAME} missing from layout ({layout_dir})"

        extract_error = self._extract_debugger_msi(msi_path, install_root)
        if extract_error:
            return None, extract_error

        try:
            if not sentinel.exists():
                return None, f"debugger executable not found at {sentinel}"
        except OSError as exc:
            return None, f"failed to verify debugger installation at {sentinel}: {exc}"

        self._downloaded_windows_debugger_root = debugger_root
        return debugger_root, ''

    def _ensure_winsdk_layout(self, layout_dir: Path) -> Optional[str]:
        """Run winsdksetup.exe to produce an offline layout containing the debugger MSI."""

        installer_path, installer_error = self._ensure_winsdk_installer()
        if installer_error:
            return installer_error
        assert installer_path is not None

        if layout_dir.exists():
            shutil.rmtree(layout_dir, ignore_errors=True)

        try:
            layout_dir.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return f"failed to create layout directory {layout_dir}: {exc}"

        log_path = layout_dir.parent / 'winsdksetup-layout.log'
        command = [
            str(installer_path),
            '/quiet',
            '/norestart',
            '/layout',
            str(layout_dir),
            '/features',
            WINSDK_FEATURE_ID,
            '/log',
            str(log_path),
        ]

        try:
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=3600,
            )
        except (subprocess.SubprocessError, OSError) as exc:
            return f"winsdksetup.exe failed to create layout: {exc}"

        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            return (
                f"winsdksetup.exe layout failed with exit code {result.returncode}: "
                f"{detail or f'see {log_path} for details'}"
            )

        return None

    def _ensure_winsdk_installer(self) -> Tuple[Optional[Path], Optional[str]]:
        """Download winsdksetup.exe if needed."""

        cache_dir = self._get_windows_debugger_cache_dir()
        installer_path = cache_dir / 'winsdksetup.exe'
        if installer_path.exists():
            return installer_path, None

        download_error = self._download_file(WINSDK_INSTALLER_URL, installer_path)
        if download_error:
            return None, f"failed to download WinSDK installer: {download_error}"

        return installer_path, None

    def _extract_debugger_msi(self, msi_path: Path, destination: Path) -> Optional[str]:
        """Extract the Debugging Tools MSI into the destination directory."""

        if destination.exists():
            shutil.rmtree(destination, ignore_errors=True)

        extract_error = self._extract_msi_package(msi_path, destination)
        if extract_error:
            shutil.rmtree(destination, ignore_errors=True)
            return extract_error

        return None

    def _get_windows_debugger_cache_dir(self) -> Path:
        """Return the directory that stores downloaded debugger assets."""

        override = os.environ.get(WINDOWS_DEBUGGER_CACHE_ENV)
        if override:
            return Path(override)

        local_app_data = os.environ.get('LOCALAPPDATA')
        if local_app_data:
            base_dir = Path(local_app_data)
        else:
            base_dir = Path.home() / 'AppData' / 'Local'

        return base_dir / 'sintra' / 'debugger_cache'

    def _download_file(self, url: str, destination: Path) -> Optional[str]:
        """Download a URL to a file atomically."""

        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return f"failed to create directory for {destination}: {exc}"

        temp_file = tempfile.NamedTemporaryFile(delete=False, dir=str(destination.parent), suffix='.tmp')
        temp_path = Path(temp_file.name)
        try:
            with urllib.request.urlopen(url) as response:
                chunk_size = 1024 * 1024
                while True:
                    chunk = response.read(chunk_size)
                    if not chunk:
                        break
                    temp_file.write(chunk)
            temp_file.close()
            os.replace(temp_path, destination)
            return None
        except urllib.error.URLError as exc:
            temp_file.close()
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass
            return f"failed to download {url}: {exc}"
        except OSError as exc:
            temp_file.close()
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass
            return f"failed to write {destination}: {exc}"

    def _extract_msi_package(self, package: Path, destination: Path) -> Optional[str]:
        """Extract an MSI into the provided destination directory."""

        msiexec = shutil.which('msiexec')
        if not msiexec:
            windir = os.environ.get('WINDIR')
            if windir:
                candidate = Path(windir) / 'System32' / 'msiexec.exe'
                if candidate.exists():
                    msiexec = str(candidate)

        if not msiexec:
            return "'msiexec' not available to extract debugger package"

        try:
            destination.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return f"failed to create extraction directory {destination}: {exc}"

        try:
            result = subprocess.run(
                [
                    msiexec,
                    '/a',
                    str(package),
                    '/qn',
                    f'TARGETDIR={destination}',
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=900,
            )
        except (subprocess.SubprocessError, OSError) as exc:
            return f"msiexec failed to extract debugger package: {exc}"

        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            return f"msiexec exited with {result.returncode}: {detail}"

        return None

    def _find_downloaded_debugger_executable(
        self,
        debugger_root: Path,
        executable_names: List[str],
    ) -> Optional[Path]:
        """Search the downloaded debugger payload for a specific executable."""

        candidate_dirs = [
            debugger_root / 'x64',
            debugger_root / 'amd64',
            debugger_root / 'dbg' / 'amd64',
            debugger_root / 'bin' / 'x64',
            debugger_root,
        ]

        for directory in candidate_dirs:
            try:
                if not directory.exists():
                    continue
            except OSError:
                continue

            for name in executable_names:
                candidate = directory / name
                try:
                    if candidate.exists():
                        return candidate
                except OSError:
                    continue

        for name in executable_names:
            try:
                matches = list(debugger_root.rglob(name))
            except OSError:
                matches = []
            if matches:
                return matches[0]

        return None

    def _prepare_windows_debuggers(self) -> None:
        """Ensure Windows debuggers are resolved before running any tests."""

        debugger, path, error = self._resolve_windows_debugger()
        if path and self.verbose:
            print(
                f"{Color.BLUE}Using Windows debugger '{debugger}' at {path}{Color.RESET}"
            )
        elif error:
            print(
                f"{Color.YELLOW}Warning: {error}. "
                f"Stack capture may be unavailable.{Color.RESET}"
            )

    def _ensure_windows_local_dumps(self) -> Optional[str]:
        """Configure Windows Error Reporting to create crash dumps for tests."""

        if sys.platform != 'win32':
            return None

        if self._windows_crash_dump_dir is not None:
            return None

        try:
            import winreg  # type: ignore
        except ImportError as exc:  # pragma: no cover - Windows specific
            return f"winreg unavailable: {exc}"

        reg_subkey = r"Software\Microsoft\Windows\Windows Error Reporting\LocalDumps"
        desired_folder_value = r"%LOCALAPPDATA%\CrashDumps"

        access_flags = winreg.KEY_READ | winreg.KEY_SET_VALUE
        try:
            key = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, reg_subkey, 0, access_flags)
        except OSError as exc:
            return f"failed to open registry key HKCU\\{reg_subkey}: {exc}"

        with key:
            local_app_data = os.environ.get('LOCALAPPDATA')
            if local_app_data:
                default_dump_dir = Path(local_app_data) / 'CrashDumps'
                folder_value_to_set = desired_folder_value
                folder_value_type = winreg.REG_EXPAND_SZ
            else:
                default_dump_dir = Path.home() / 'AppData' / 'Local' / 'CrashDumps'
                folder_value_to_set = str(default_dump_dir)
                folder_value_type = winreg.REG_SZ

            existing_folder_value: Optional[str]
            try:
                existing_folder_value, existing_type = winreg.QueryValueEx(key, "DumpFolder")
            except FileNotFoundError:
                existing_folder_value = None

            dump_dir: Path
            if existing_folder_value:
                expanded = os.path.expandvars(existing_folder_value)
                dump_dir = Path(expanded).expanduser()
                if not dump_dir.is_absolute():
                    dump_dir = default_dump_dir
            else:
                dump_dir = default_dump_dir
                try:
                    winreg.SetValueEx(
                        key,
                        "DumpFolder",
                        0,
                        folder_value_type,
                        folder_value_to_set,
                    )
                except OSError as exc:
                    return f"failed to set DumpFolder value: {exc}"

            value_expectations = (
                ("DumpType", 2, winreg.REG_DWORD),
                ("DumpCount", 10, winreg.REG_DWORD),
            )
            for value_name, expected, reg_type in value_expectations:
                try:
                    current_value, _ = winreg.QueryValueEx(key, value_name)
                except FileNotFoundError:
                    current_value = None
                if current_value != expected:
                    try:
                        winreg.SetValueEx(key, value_name, 0, reg_type, expected)
                    except OSError as exc:
                        return f"failed to set {value_name} value: {exc}"

        try:
            dump_dir.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return f"failed to create crash dump directory {dump_dir}: {exc}"

        self._windows_crash_dump_dir = dump_dir
        return None

    def _configure_windows_jit_debugging(self) -> Optional[str]:
        """Disable intrusive JIT prompts so crash dumps complete automatically."""

        if sys.platform != 'win32':
            return None

        try:
            import winreg  # type: ignore
        except ImportError as exc:  # pragma: no cover - Windows specific
            return f"winreg unavailable: {exc}"

        errors: List[str] = []

        def set_dword(
            root: "winreg.HKEYType",
            subkey: str,
            value_name: str,
            value: int,
            access: int,
        ) -> None:
            try:
                key = winreg.CreateKeyEx(root, subkey, 0, access)
            except OSError as exc:
                errors.append(f"failed to open {subkey}: {exc}")
                return

            try:
                with key:
                    winreg.SetValueEx(key, value_name, 0, winreg.REG_DWORD, value)
            except OSError as exc:
                errors.append(f"failed to set {subkey}\\{value_name}: {exc}")

        set_dword(
            winreg.HKEY_LOCAL_MACHINE,
            r"Software\Microsoft\Windows NT\CurrentVersion\AeDebug",
            "Auto",
            1,
            winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY,
        )

        set_dword(
            winreg.HKEY_LOCAL_MACHINE,
            r"Software\Microsoft\Windows\Windows Error Reporting",
            "DontShowUI",
            1,
            winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY,
        )

        jit_subkeys = [
            (winreg.HKEY_LOCAL_MACHINE, r"Software\Microsoft\.NETFramework"),
            (winreg.HKEY_LOCAL_MACHINE, r"Software\Wow6432Node\Microsoft\.NETFramework"),
            (winreg.HKEY_CURRENT_USER, r"Software\Microsoft\.NETFramework"),
        ]
        for root, subkey in jit_subkeys:
            access = winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY
            if root == winreg.HKEY_CURRENT_USER:
                access = winreg.KEY_SET_VALUE
            elif "Wow6432Node" in subkey:
                access = winreg.KEY_SET_VALUE | winreg.KEY_WOW64_32KEY
            set_dword(root, subkey, "DbgJITDebugLaunchSetting", 2, access)

        if errors:
            return "; ".join(errors)

        return None

    def _resolve_windows_debugger(self) -> Tuple[Optional[str], Optional[str], str]:
        """Return the first available Windows debugger and its path."""

        debugger_candidates = ['cdb', 'ntsd', 'windbg']
        errors: List[str] = []

        for debugger in debugger_candidates:
            path, error = self._locate_windows_debugger(debugger)
            if path:
                return debugger, path, ''
            if error:
                errors.append(f"{debugger}: {error}")

        if errors:
            return None, None, '; '.join(errors)

        return None, None, 'no Windows debugger available'

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

        if self._windows_crash_dump_dir:
            candidate_dirs.insert(0, self._windows_crash_dump_dir)

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
        fallback_outputs: List[Tuple[str, str, int, str]] = []

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

            stdout = result.stdout.strip()
            stderr = result.stderr.strip()

            output = stdout
            output_from_stderr = False
            if not output and stderr:
                output = stderr
                output_from_stderr = True

            normalized_code = self._normalize_windows_returncode(result.returncode)
            exit_ok = normalized_code in self._WINDOWS_DEBUGGER_SUCCESS_CODES

            if exit_ok:
                if output:
                    note = ""
                    if normalized_code != 0:
                        note = (
                            f"\n\n[Debugger exited with code {self._format_windows_returncode(normalized_code)};"
                            " treated as success]"
                        )
                    stack_outputs.append(f"dump file: {dump_path}\n{output}{note}")
                    break
                continue

            if output:
                if not output_from_stderr:
                    fallback_outputs.append((f"dump file: {dump_path}", output, normalized_code, stderr))
                else:
                    capture_errors.append(
                        self._format_windows_debugger_failure(
                            debugger_name,
                            dump_path.name,
                            normalized_code,
                            stderr,
                        )
                    )
            else:
                capture_errors.append(
                    self._format_windows_debugger_failure(
                        debugger_name,
                        dump_path.name,
                        normalized_code,
                        stderr,
                    )
                )

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if fallback_outputs:
            annotated = []
            for label, output, normalized_code, stderr in fallback_outputs:
                detail = f"; stderr: {stderr}" if stderr else ""
                annotated.append(
                    f"{label}\n{output}\n\n[Debugger exited with code {self._format_windows_returncode(normalized_code)}; output may be incomplete{detail}]"
                )
            return "\n\n".join(annotated), "; ".join(capture_errors) if capture_errors else ""

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
        fallback_outputs: List[Tuple[str, str, int, str]] = []

        for target_pid in sorted(set(target_pids)):
            try:
                command = [debugger_path]
                if debugger_name == 'windbg':
                    command.append('-Q')
                command.extend(['-pv', '-p', str(target_pid), '-c', '.symfix; .reload; ~* k; qd'])

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

            stdout = result.stdout.strip()
            stderr = result.stderr.strip()

            output = stdout
            output_from_stderr = False
            if not output and stderr:
                output = stderr
                output_from_stderr = True

            normalized_code = self._normalize_windows_returncode(result.returncode)
            exit_ok = normalized_code in self._WINDOWS_DEBUGGER_SUCCESS_CODES

            if exit_ok:
                if output:
                    note = ""
                    if normalized_code != 0:
                        note = (
                            f"\n\n[Debugger exited with code {self._format_windows_returncode(normalized_code)};"
                            " treated as success]"
                        )
                    stack_outputs.append(f"PID {target_pid}\n{output}{note}")
                continue

            if output:
                if not output_from_stderr:
                    fallback_outputs.append((f"PID {target_pid}", output, normalized_code, stderr))
                else:
                    capture_errors.append(
                        self._format_windows_debugger_failure(
                            debugger_name,
                            f"PID {target_pid}",
                            normalized_code,
                            stderr,
                        )
                    )
            else:
                capture_errors.append(
                    self._format_windows_debugger_failure(
                        debugger_name,
                        f"PID {target_pid}",
                        normalized_code,
                        stderr,
                    )
                )

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if fallback_outputs:
            annotated = []
            for label, output, normalized_code, stderr in fallback_outputs:
                detail = f"; stderr: {stderr}" if stderr else ""
                annotated.append(
                    f"{label}\n{output}\n\n[Debugger exited with code {self._format_windows_returncode(normalized_code)}; output may be incomplete{detail}]"
                )
            return "\n\n".join(annotated), "; ".join(capture_errors) if capture_errors else ""

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no stack data captured"

    @staticmethod
    def _normalize_windows_returncode(returncode: int) -> int:
        """Normalize signed Windows return codes to their unsigned 32-bit representation."""

        return returncode & 0xFFFFFFFF

    @staticmethod
    def _format_windows_returncode(returncode: int) -> str:
        """Format a Windows return code for display."""

        return f"0x{returncode:08X}"

    @classmethod
    def _format_windows_debugger_failure(
        cls,
        debugger_name: str,
        target: str,
        returncode: int,
        stderr: str,
    ) -> str:
        detail = f": {stderr.strip()}" if stderr else ""
        return (
            f"{target}: {debugger_name} exited with code {cls._format_windows_returncode(returncode)}{detail}"
        )

    _WINDOWS_DEBUGGER_SUCCESS_CODES = {0x00000000, 0xD000010A}

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
    try:
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
                name: _calculate_target_repetitions(args.repetitions, weight)
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

                lingering = _find_lingering_processes(("sintra_",))
                if lingering:
                    details = _describe_processes(lingering)
                    print(
                        f"  {Color.YELLOW}Warning: Detected lingering sintra processes before starting the round: {details}{Color.RESET}"
                    )

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

                round_elapsed = time.time() - suite_start_time
                print(
                    f"    {Color.BLUE}Round complete - total elapsed: {format_duration(round_elapsed)}{Color.RESET}"
                )

                available_memory = _available_memory_bytes()
                disk_space = _available_disk_bytes(runner.build_dir)
                print(
                    f"    Diagnostics: available memory={_format_size(available_memory)}, "
                    f"free disk={_format_size(disk_space)}"
                )

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
