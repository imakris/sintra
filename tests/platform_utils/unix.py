"""Unix/Linux-specific platform utilities."""
from __future__ import annotations

import importlib
import importlib.util
import os
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

from .base import PlatformUtils

# Try to import psutil if available
_PSUTIL = None
if importlib.util.find_spec("psutil") is not None:
    _PSUTIL = importlib.import_module("psutil")

# Try to import signal module
try:
    import signal as _signal_module
except ImportError:
    _signal_module = None

# Collect POSIX stop signals
_POSIX_STOP_SIGNALS: Set[int] = set()
if _signal_module is not None:
    for _sig_name in ("SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU"):
        _sig_value = getattr(_signal_module, _sig_name, None)
        if isinstance(_sig_value, int):
            _POSIX_STOP_SIGNALS.add(_sig_value)


class UnixPlatformUtils(PlatformUtils):
    """Unix/Linux-specific implementations of platform utilities."""

    def get_available_memory_bytes(self) -> Optional[int]:
        """Return available memory in bytes using os.sysconf or psutil."""
        if _PSUTIL is not None:
            try:
                return _PSUTIL.virtual_memory().available
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

        return None

    def get_core_dump_search_directories(
        self,
        test_path: Path,
        build_dir: Path,
        scratch_base: Path,
        cwd: Path,
    ) -> List[Path]:
        """Return directories that may contain core dumps."""
        candidates: Set[Path] = {
            test_path.parent.resolve(),
            cwd.resolve(),
            build_dir.resolve(),
            scratch_base,
        }

        # Add Linux-specific core dump location
        if sys.platform.startswith("linux"):
            candidates.add(Path("/var/lib/systemd/coredump"))

        return [path for path in candidates if path]

    def normalize_executable_name(self, name: str) -> str:
        """Normalize executable name (no-op on Unix)."""
        return name

    def get_process_creation_kwargs(self, base_kwargs: Dict[str, Any]) -> Dict[str, Any]:
        """Return Unix-specific kwargs for subprocess.Popen."""
        kwargs = base_kwargs.copy()
        kwargs['start_new_session'] = True
        return kwargs

    def can_terminate_process_group(self) -> bool:
        """Unix supports process group termination."""
        return True

    def kill_process_tree(self, pid: int) -> None:
        """Kill a process and all its children using process groups."""
        try:
            if _signal_module is None:
                return

            try:
                pgid = os.getpgid(pid)
            except ProcessLookupError:
                pgid = None

            if pgid is not None:
                os.killpg(pgid, _signal_module.SIGKILL)
            else:
                os.kill(pid, _signal_module.SIGKILL)
        except Exception as e:
            if self.verbose:
                print(f"Warning: Failed to kill process {pid}: {e}", file=sys.stderr)

    def kill_sintra_processes(self, test_names: List[str]) -> None:
        """Kill all existing sintra test processes using pkill."""
        try:
            # Use pkill to kill all processes matching 'sintra'
            subprocess.run(
                ['pkill', '-9', 'sintra'],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5
            )
        except Exception:
            # Ignore errors - processes may not exist
            pass

    def is_crash_exit(self, returncode: int) -> bool:
        """Return True if the exit code represents an abnormal termination."""
        if returncode == 0:
            return False

        signal_num = self.decode_signal(returncode)
        if signal_num is not None and signal_num in _POSIX_STOP_SIGNALS:
            return False

        # POSIX: negative return codes indicate termination by signal
        # Codes > 128 are also commonly used to report signal-based exits
        return returncode < 0 or returncode > 128

    def decode_signal(self, returncode: int) -> Optional[int]:
        """Return the POSIX signal number encoded in a return code."""
        if returncode == 0:
            return None

        if returncode < 0:
            return -returncode

        if returncode > 128:
            signal_num = returncode - 128
            if signal_num > 0:
                return signal_num
        return None

    def supports_signals(self) -> bool:
        """Unix supports signals."""
        return True

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        """Return all process IDs belonging to the provided process group."""
        pids: List[int] = []

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

    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Return all descendant process IDs for the provided root PID."""
        descendants: List[int] = []

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

            remainder = stat_content[close_paren + 2:].split()
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

    def snapshot_process_table(self) -> List[tuple[int, int, int]]:
        """Return (pid, ppid, pgid) tuples using the ps command."""
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

        snapshot: List[tuple[int, int, int]] = []
        for line in result.stdout.splitlines():
            parts = line.split()
            if len(parts) != 3:
                continue
            try:
                pid = int(parts[0])
                ppid = int(parts[1])
                pgid = int(parts[2])
                snapshot.append((pid, ppid, pgid))
            except ValueError:
                continue

        return snapshot

    def _collect_process_group_pids_via_ps(self, pgid: int) -> List[int]:
        """Collect process IDs that belong to the provided PGID using ps."""
        snapshot = self.snapshot_process_table()
        if not snapshot:
            return []

        return [pid for pid, _, process_group in snapshot if process_group == pgid]

    def _collect_descendant_pids_via_ps(self, root_pid: int) -> List[int]:
        """Collect descendant process IDs for the provided root PID using ps."""
        snapshot = self.snapshot_process_table()
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
