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
from functools import partial
import os
import shutil
import subprocess
import sys
import threading
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR.parent) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR.parent))

from typing import Any, Dict, IO, Iterable, List, Optional, Sequence, Set, Tuple

from tests.debuggers import DebuggerStrategy, get_debugger_strategy

print = partial(__import__("builtins").print, file=sys.stderr, flush=True)


def _instrumentation_print(message: str) -> None:
    """Emit high-frequency instrumentation to stdout with explicit flushing."""

    sys.stdout.write(f"{message}\n")
    sys.stdout.flush()
    time.sleep(0.001)

_PSUTIL = None
if importlib.util.find_spec("psutil") is not None:
    _PSUTIL = importlib.import_module("psutil")


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
                 preserve_on_timeout: bool = False, test_subdir: str = None):
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
        self._stack_capture_history: Dict[str, Set[str]] = defaultdict(set)
        self._stack_capture_history_lock = threading.Lock()
        self._preserve_core_dumps = _env_flag(PRESERVE_CORES_ENV)
        self._core_cleanup_lock = threading.Lock()
        self._core_cleanup_messages: List[Tuple[str, str]] = []
        self._core_cleanup_bytes_freed = 0
        self._scratch_cleanup_lock = threading.Lock()
        self._scratch_cleanup_dirs_removed = 0
        self._scratch_cleanup_bytes_freed = 0
        self._instrumentation_enabled = True
        self._instrumentation_disk_root: Optional[Path] = self.build_dir
        self._instrument_disk_cache: Dict[Path, Tuple[float, str]] = {}

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

        # If test_subdir is specified, append it to the test directory
        if test_subdir:
            self.test_dir = self.test_dir / test_subdir

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
                free_bytes = _available_disk_bytes(cache_key)
                formatted = _format_size(free_bytes)
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
            ordered = sorted(
                enumerate(tests),
                key=lambda item: (
                    0
                    if _canonical_test_name(item[1].name).startswith("dummy_test")
                    else 1,
                    item[0],
                ),
            )
            test_suites[config] = [invocation for _, invocation in ordered]

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
        instrumentation_active = self.instrumentation_active()
        instrument = partial(self._instrument_step, disk_path=self.build_dir)
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
            threads: List[Tuple[threading.Thread, IO[str], str, Optional[int]]] = []
            reader_state_lock = threading.Lock()
            reader_states: Dict[str, Dict[str, Any]] = {}
            process_group_id: Optional[int] = None

            def terminate_process_group_members(reason: str) -> bool:
                """Ensure helpers in the spawned process group terminate."""

                if sys.platform == 'win32':
                    return False

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

            def monitor_stream(stream, buffer: List[str], descriptor: str) -> None:
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
                terminate_process_group_members(f"{invocation.name} exit")
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
        return self._debugger.capture_process_stacks(pid, process_group)

    def _capture_core_dump_stack(
        self,
        invocation: TestInvocation,
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        return self._debugger.capture_core_dump_stack(invocation, start_time, pid)

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

    def _describe_pids(self, pids: Iterable[int]) -> Dict[int, str]:
        """Return human-readable details for the provided process IDs."""

        unique_pids = sorted({pid for pid in pids if isinstance(pid, int) and pid > 0})
        if not unique_pids:
            return {}

        details: Dict[int, str] = {}

        if _PSUTIL is not None:
            psutil_module = _PSUTIL
            now = time.time()
            for pid in unique_pids:
                if pid in details:
                    continue
                try:
                    proc = psutil_module.Process(pid)
                except Exception:
                    continue

                parts: List[str] = []
                try:
                    parts.append(f"ppid={proc.ppid()}")
                except Exception:
                    pass
                try:
                    parts.append(f"pgid={os.getpgid(pid)}")
                except Exception:
                    pass
                try:
                    parts.append(f"status={proc.status()}")
                except Exception:
                    pass
                try:
                    create_time = proc.create_time()
                except Exception:
                    create_time = None
                if create_time:
                    uptime = now - create_time
                    if uptime >= 0:
                        parts.append(f"uptime={uptime:.1f}s")
                try:
                    cmdline = proc.cmdline()
                except Exception:
                    cmdline = []
                if not cmdline:
                    try:
                        name = proc.name()
                    except Exception:
                        name = ""
                    if name:
                        cmdline = [name]
                if cmdline:
                    parts.append(f"cmd={' '.join(cmdline)}")
                if parts:
                    details[pid] = " ".join(parts)

        remaining = [pid for pid in unique_pids if pid not in details]
        ps_executable = shutil.which('ps')
        if remaining and ps_executable:
            chunk_size = 16
            for offset in range(0, len(remaining), chunk_size):
                chunk = remaining[offset : offset + chunk_size]
                command = [
                    ps_executable,
                    '-o',
                    'pid=,ppid=,pgid=,stat=,etime=,command=',
                    '-p',
                    ','.join(str(pid) for pid in chunk),
                ]
                try:
                    result = subprocess.run(
                        command,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        check=True,
                        timeout=5,
                    )
                except subprocess.SubprocessError:
                    continue

                for line in result.stdout.splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    fields = line.split(None, 5)
                    if len(fields) < 5:
                        continue
                    try:
                        pid_value = int(fields[0])
                    except ValueError:
                        continue
                    description_parts = [
                        f"ppid={fields[1]}",
                        f"pgid={fields[2]}",
                        f"stat={fields[3]}",
                        f"etime={fields[4]}",
                    ]
                    if len(fields) >= 6:
                        description_parts.append(f"cmd={fields[5]}")
                    details[pid_value] = " ".join(description_parts)

        for pid in unique_pids:
            details.setdefault(pid, 'details unavailable')

        return details

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
    parser.add_argument('--test-dir', type=str, default=None,
                        help='Subdirectory within build-dir/tests to search for tests (e.g., "manual" for manual tests)')
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
    if args.test_dir:
        print(f"Test subdirectory: {args.test_dir}")
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
    runner = TestRunner(build_dir, args.config, args.timeout, args.verbose, preserve_on_timeout, test_subdir=args.test_dir)
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

            # Print test plan table for this suite
            print("\n  Test order:")

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

                lingering = _find_lingering_processes(("sintra_",))
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
