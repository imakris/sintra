"""Platform-specific operations for the test runner.

This module provides a platform abstraction layer for operations that differ
between Windows, Linux, and macOS. It follows the same pattern as the debuggers
module with a base class and platform-specific implementations.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple, TYPE_CHECKING

if TYPE_CHECKING:
    pass

# Conditionally import psutil if available
_PSUTIL = None
try:
    import importlib.util
    if importlib.util.find_spec("psutil") is not None:
        import importlib
        _PSUTIL = importlib.import_module("psutil")
except Exception:
    pass

# Import signal module if available (not on all platforms)
_signal_module = None
try:
    import signal as _signal_module
except ImportError:
    pass

# POSIX stop signals set
_POSIX_STOP_SIGNALS: Set[int] = set()
if _signal_module is not None:
    for _sig_name in ("SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU"):
        _sig_value = getattr(_signal_module, _sig_name, None)
        if isinstance(_sig_value, int):
            _POSIX_STOP_SIGNALS.add(_sig_value)


class PlatformOps(ABC):
    """Abstract base class for platform-specific operations."""

    def __init__(self, verbose: bool = False):
        self.verbose = verbose

    @abstractmethod
    def get_available_memory_bytes(self) -> Optional[int]:
        """Return available physical memory in bytes, or None if unavailable."""

    @abstractmethod
    def get_core_dump_directories(self, base_dirs: List[Path]) -> List[Path]:
        """Return directories that may contain core dumps.

        Args:
            base_dirs: Base directories to always include (e.g., cwd, build_dir).

        Returns:
            List of directories to search for core dumps.
        """

    @abstractmethod
    def normalize_executable_name(self, name: str) -> str:
        """Normalize executable name (e.g., strip .exe on Windows).

        Args:
            name: The executable name to normalize.

        Returns:
            Normalized executable name.
        """

    @abstractmethod
    def get_subprocess_kwargs(self) -> Dict[str, any]:
        """Return platform-specific kwargs for subprocess.Popen to create a new process group.

        Returns:
            Dictionary of kwargs to pass to subprocess.Popen.
        """

    @abstractmethod
    def collect_process_group_pids(self, pgid: int) -> List[int]:
        """Return all process IDs belonging to the provided process group.

        Args:
            pgid: Process group ID.

        Returns:
            List of PIDs in the process group.
        """

    @abstractmethod
    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Return all descendant process IDs for the provided root PID.

        Args:
            root_pid: Root process ID.

        Returns:
            List of descendant PIDs.
        """

    @abstractmethod
    def kill_process_tree(self, pid: int) -> None:
        """Kill a process and all its children.

        Args:
            pid: Process ID to kill.
        """

    @abstractmethod
    def kill_all_test_processes(self, test_names: List[str]) -> None:
        """Kill all running test processes.

        Args:
            test_names: List of test executable names.
        """

    @abstractmethod
    def is_crash_exit(self, returncode: int) -> bool:
        """Return True if the exit code represents an abnormal termination.

        Args:
            returncode: Process exit code.

        Returns:
            True if the exit represents a crash.
        """

    @abstractmethod
    def decode_posix_signal(self, returncode: int) -> Optional[int]:
        """Return the POSIX signal number encoded in a return code.

        Args:
            returncode: Process exit code.

        Returns:
            Signal number or None.
        """

    @abstractmethod
    def format_signal_name(self, signal_num: int) -> str:
        """Convert signal number to human-readable name.

        Args:
            signal_num: Signal number.

        Returns:
            Human-readable signal name.
        """

    @abstractmethod
    def supports_process_groups(self) -> bool:
        """Return True if the platform supports process groups."""

    @abstractmethod
    def get_process_group_id(self, pid: int) -> Optional[int]:
        """Get the process group ID for a given PID.

        Args:
            pid: Process ID.

        Returns:
            Process group ID or None if unavailable.
        """


class WindowsPlatformOps(PlatformOps):
    """Windows-specific platform operations."""

    def get_available_memory_bytes(self) -> Optional[int]:
        """Return available physical memory using psutil."""
        if _PSUTIL is not None:
            try:
                return int(_PSUTIL.virtual_memory().available)
            except Exception:
                pass
        return None

    def get_core_dump_directories(self, base_dirs: List[Path]) -> List[Path]:
        """Windows uses WER (Windows Error Reporting) for crash dumps.

        Core dumps are not typically in predictable locations on Windows.
        Return only the base directories.
        """
        return base_dirs

    def normalize_executable_name(self, name: str) -> str:
        """Strip .exe suffix (case-insensitive) from executable name."""
        if name.lower().endswith('.exe'):
            return name[:-4]
        return name

    def get_subprocess_kwargs(self) -> Dict[str, any]:
        """Return kwargs to create a new process group on Windows."""
        creationflags = 0
        if hasattr(subprocess, 'CREATE_NEW_PROCESS_GROUP'):
            creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
        return {'creationflags': creationflags}

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        """Windows doesn't support POSIX process groups."""
        return []

    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Collect descendant PIDs on Windows using psutil or PowerShell fallback."""
        descendants: List[int] = []

        # Try psutil first
        psutil_worked = False
        if _PSUTIL is not None:
            try:
                root_proc = _PSUTIL.Process(root_pid)
                children = root_proc.children(recursive=True)
                descendants = [child.pid for child in children]
                psutil_worked = True
                if self.verbose:
                    print(f"[DEBUG] collect_descendant_pids({root_pid}) found {len(descendants)} descendants via psutil: {descendants}", file=sys.stderr)
            except (_PSUTIL.NoSuchProcess, _PSUTIL.AccessDenied) as e:
                if self.verbose:
                    print(f"[DEBUG] collect_descendant_pids({root_pid}) psutil failed: {e}, trying Win32_Process fallback", file=sys.stderr)
            except Exception as e:
                if self.verbose:
                    print(f"[DEBUG] collect_descendant_pids({root_pid}) psutil unexpected error ({type(e).__name__}): {e}, trying Win32_Process fallback", file=sys.stderr)

        # Fallback to PowerShell Win32_Process query
        if not psutil_worked:
            descendants = self._collect_descendant_pids_windows_fallback(root_pid)
            if self.verbose and descendants:
                print(f"[DEBUG] collect_descendant_pids({root_pid}) found {len(descendants)} descendants via Win32_Process: {descendants}", file=sys.stderr)

        return descendants

    def _collect_descendant_pids_windows_fallback(self, root_pid: int) -> List[int]:
        """Use PowerShell to query Win32_Process for descendants."""
        # Validate root_pid
        if not isinstance(root_pid, int):
            if self.verbose:
                print(f"[DEBUG] _collect_descendant_pids_windows_fallback: invalid root_pid type: {type(root_pid)}", file=sys.stderr)
            return []

        if root_pid <= 0 or root_pid > 0xFFFFFFFF:
            if self.verbose:
                print(f"[DEBUG] _collect_descendant_pids_windows_fallback: root_pid out of range: {root_pid}", file=sys.stderr)
            return []

        powershell = shutil.which("powershell")
        if not powershell:
            if self.verbose:
                print("[DEBUG] _collect_descendant_pids_windows_fallback: powershell not found", file=sys.stderr)
            return []

        # Query all processes to build parent-child map
        script = r"""
            $AllProcesses = Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId;
            $AllProcesses | ForEach-Object { "$($_.ProcessId),$($_.ParentProcessId)" }
        """

        try:
            result = subprocess.run(
                [powershell, "-NoProfile", "-NonInteractive", "-Command", script],
                capture_output=True,
                text=True,
                timeout=5,
                creationflags=subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0
            )
        except subprocess.TimeoutExpired:
            if self.verbose:
                print("[DEBUG] _collect_descendant_pids_windows_fallback: PowerShell query timed out", file=sys.stderr)
            return []
        except Exception as e:
            if self.verbose:
                print(f"[DEBUG] _collect_descendant_pids_windows_fallback: PowerShell query failed: {e}", file=sys.stderr)
            return []

        if result.returncode != 0:
            if self.verbose:
                print(f"[DEBUG] _collect_descendant_pids_windows_fallback: PowerShell returned {result.returncode}", file=sys.stderr)
            return []

        # Build parent-child map
        parent_to_children: Dict[int, List[int]] = {}
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line or ',' not in line:
                continue
            parts = line.split(',')
            if len(parts) != 2:
                continue
            try:
                pid = int(parts[0])
                ppid = int(parts[1])
                parent_to_children.setdefault(ppid, []).append(pid)
            except ValueError:
                continue

        # Recursively collect descendants
        descendants: List[int] = []
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

    def kill_process_tree(self, pid: int) -> None:
        """Kill a process and all its children using taskkill."""
        try:
            subprocess.run(
                ['taskkill', '/F', '/T', '/PID', str(pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5
            )
        except Exception:
            pass

    def kill_all_test_processes(self, test_names: List[str]) -> None:
        """Kill all test processes by name using taskkill."""
        for test_name in test_names:
            # Ensure .exe extension
            if not test_name.lower().endswith('.exe'):
                test_name = test_name + '.exe'
            try:
                subprocess.run(
                    ['taskkill', '/F', '/IM', test_name],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=5
                )
            except Exception:
                pass

    def is_crash_exit(self, returncode: int) -> bool:
        """Check if exit code represents a Windows crash.

        Windows crash codes are typically large unsigned values such as 0xC0000005.
        """
        if returncode == 0:
            return False
        return returncode < 0 or returncode >= 0xC0000000

    def decode_posix_signal(self, returncode: int) -> Optional[int]:
        """Windows doesn't use POSIX signals."""
        return None

    def format_signal_name(self, signal_num: int) -> str:
        """Format signal name (Windows doesn't have signals)."""
        return f"signal {signal_num}"

    def supports_process_groups(self) -> bool:
        """Windows doesn't support POSIX process groups."""
        return False

    def get_process_group_id(self, pid: int) -> Optional[int]:
        """Windows doesn't support POSIX process groups."""
        return None


class UnixPlatformOps(PlatformOps):
    """Unix/Linux/macOS platform operations."""

    def get_available_memory_bytes(self) -> Optional[int]:
        """Return available physical memory using psutil, sysconf, or vm_stat."""
        # Try psutil first
        if _PSUTIL is not None:
            try:
                return int(_PSUTIL.virtual_memory().available)
            except Exception:
                pass

        # Try POSIX sysconf
        try:
            page_size = os.sysconf("SC_PAGE_SIZE")
            avail_pages = os.sysconf("SC_AVPHYS_PAGES")
            if isinstance(page_size, int) and isinstance(avail_pages, int):
                return page_size * avail_pages
        except (AttributeError, OSError, ValueError):
            pass

        # macOS-specific: try vm_stat command
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

            # Parse page size
            page_size_bytes = 4096
            for line in vm_stat.stdout.splitlines():
                if line.startswith("page size of"):
                    parts = line.split()
                    try:
                        page_size_bytes = int(parts[3])
                    except (IndexError, ValueError):
                        page_size_bytes = 4096
                    break

            # Parse memory statistics
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

    def get_core_dump_directories(self, base_dirs: List[Path]) -> List[Path]:
        """Return directories that may contain core dumps on Unix systems."""
        candidates: Set[Path] = set(base_dirs)

        if sys.platform == "darwin":
            candidates.add(Path("/cores"))
            candidates.add(Path.home() / "Library" / "Logs" / "DiagnosticReports")
        elif sys.platform.startswith("linux"):
            candidates.add(Path("/var/lib/systemd/coredump"))

        return [path for path in candidates if path]

    def normalize_executable_name(self, name: str) -> str:
        """Unix executables don't need normalization."""
        return name

    def get_subprocess_kwargs(self) -> Dict[str, any]:
        """Return kwargs to create a new session on Unix."""
        return {'start_new_session': True}

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        """Collect all PIDs in a process group using /proc or ps."""
        pids: List[int] = []

        # Try /proc filesystem first (Linux)
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

    def _collect_process_group_pids_via_ps(self, pgid: int) -> List[int]:
        """Fallback to ps command for collecting process group PIDs."""
        ps_path = shutil.which('ps')
        if not ps_path:
            return []

        try:
            result = subprocess.run(
                [ps_path, '-eo', 'pid=,ppid=,pgid='],
                capture_output=True,
                text=True,
                timeout=5,
            )
        except Exception:
            return []

        if result.returncode != 0:
            return []

        pids: List[int] = []
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                pid = int(parts[0])
                candidate_pgid = int(parts[2])
                if candidate_pgid == pgid:
                    pids.append(pid)
            except ValueError:
                continue

        return pids

    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Collect descendant PIDs using /proc or ps."""
        descendants: List[int] = []

        # Try /proc filesystem first (Linux)
        proc_path = Path('/proc')
        entries: Optional[List[Path]] = None
        if proc_path.exists():
            try:
                entries = list(proc_path.iterdir())
            except Exception:
                entries = None

        if entries is None:
            return self._collect_descendant_pids_via_ps(root_pid)

        # Build parent-child map from /proc/[pid]/stat
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

            # Parse /proc/[pid]/stat format: pid (comm) state ppid ...
            close_paren = stat_content.find(')')
            if close_paren == -1 or close_paren + 2 >= len(stat_content):
                continue

            remainder = stat_content[close_paren + 2:].split()
            if len(remainder) < 2:
                continue

            try:
                parent_pid = int(remainder[1])
            except ValueError:
                continue

            parent_to_children.setdefault(parent_pid, []).append(pid)

        # Recursively collect descendants
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

    def _collect_descendant_pids_via_ps(self, root_pid: int) -> List[int]:
        """Fallback to ps command for collecting descendant PIDs."""
        ps_path = shutil.which('ps')
        if not ps_path:
            return []

        try:
            result = subprocess.run(
                [ps_path, '-eo', 'pid=,ppid=,pgid='],
                capture_output=True,
                text=True,
                timeout=5,
            )
        except Exception:
            return []

        if result.returncode != 0:
            return []

        # Build parent-child map
        parent_to_children: Dict[int, List[int]] = {}
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                pid = int(parts[0])
                ppid = int(parts[1])
                parent_to_children.setdefault(ppid, []).append(pid)
            except ValueError:
                continue

        # Recursively collect descendants
        descendants: List[int] = []
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

    def kill_process_tree(self, pid: int) -> None:
        """Kill a process and all its children using signals."""
        if _signal_module is None:
            return

        try:
            # Try to kill the process group first
            try:
                pgid = os.getpgid(pid)
            except ProcessLookupError:
                pgid = None

            if pgid is not None:
                os.killpg(pgid, _signal_module.SIGKILL)
            else:
                os.kill(pid, _signal_module.SIGKILL)
        except Exception:
            pass

    def kill_all_test_processes(self, test_names: List[str]) -> None:
        """Kill all test processes by name using pkill."""
        pkill_path = shutil.which('pkill')
        if not pkill_path:
            return

        for test_name in test_names:
            # Strip .exe if present (shouldn't be on Unix, but be defensive)
            test_name = self.normalize_executable_name(test_name)
            try:
                subprocess.run(
                    [pkill_path, '-9', '-f', test_name],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=5
                )
            except Exception:
                pass

    def is_crash_exit(self, returncode: int) -> bool:
        """Check if exit code represents a Unix crash (signal termination)."""
        if returncode == 0:
            return False

        signal_num = self.decode_posix_signal(returncode)
        if signal_num is not None and signal_num in _POSIX_STOP_SIGNALS:
            return False

        # POSIX: negative return codes indicate termination by signal.
        # Codes > 128 are also commonly used to report signal-based exits.
        return returncode < 0 or returncode > 128

    def decode_posix_signal(self, returncode: int) -> Optional[int]:
        """Extract signal number from return code."""
        if returncode == 0:
            return None

        if returncode < 0:
            return -returncode

        if returncode > 128:
            signal_num = returncode - 128
            if signal_num > 0:
                return signal_num

        return None

    def format_signal_name(self, signal_num: int) -> str:
        """Convert signal number to human-readable name."""
        if _signal_module is not None:
            try:
                return _signal_module.Signals(signal_num).name
            except Exception:
                pass
        return f"signal {signal_num}"

    def supports_process_groups(self) -> bool:
        """Unix supports process groups."""
        return True

    def get_process_group_id(self, pid: int) -> Optional[int]:
        """Get the process group ID for a given PID."""
        if not hasattr(os, 'getpgid'):
            return None
        try:
            return os.getpgid(pid)
        except (ProcessLookupError, PermissionError, OSError):
            return None


def get_platform_ops(verbose: bool = False) -> PlatformOps:
    """Return the appropriate platform operations instance for the current platform.

    Args:
        verbose: Enable verbose logging.

    Returns:
        Platform-specific PlatformOps instance.
    """
    if sys.platform == 'win32':
        return WindowsPlatformOps(verbose=verbose)
    else:
        return UnixPlatformOps(verbose=verbose)


__all__ = ["PlatformOps", "get_platform_ops"]
