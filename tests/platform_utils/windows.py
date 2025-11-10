"""Windows-specific platform utilities."""
from __future__ import annotations

import importlib
import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Set

from .base import PlatformUtils

# Try to import psutil if available
_PSUTIL = None
if importlib.util.find_spec("psutil") is not None:
    _PSUTIL = importlib.import_module("psutil")


class WindowsPlatformUtils(PlatformUtils):
    """Windows-specific implementations of platform utilities."""

    def get_available_memory_bytes(self) -> Optional[int]:
        """Return available memory in bytes using os.sysconf or psutil."""
        if _PSUTIL is not None:
            try:
                return _PSUTIL.virtual_memory().available
            except Exception:
                pass
        return None

    def get_core_dump_search_directories(
        self,
        test_path: Path,
        build_dir: Path,
        scratch_base: Path,
        cwd: Path,
    ) -> List[Path]:
        """Return directories that may contain core dumps (Windows doesn't use traditional core dumps)."""
        candidates: Set[Path] = {
            test_path.parent.resolve(),
            cwd.resolve(),
            build_dir.resolve(),
            scratch_base,
        }
        return [path for path in candidates if path]

    def normalize_executable_name(self, name: str) -> str:
        """Normalize executable name by stripping .exe extension."""
        if name.lower().endswith('.exe'):
            return name[:-4]
        return name

    def get_process_creation_kwargs(self, base_kwargs: Dict[str, Any]) -> Dict[str, Any]:
        """Return Windows-specific kwargs for subprocess.Popen."""
        kwargs = base_kwargs.copy()
        creationflags = 0
        if hasattr(subprocess, 'CREATE_NEW_PROCESS_GROUP'):
            creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
        kwargs['creationflags'] = creationflags
        return kwargs

    def can_terminate_process_group(self) -> bool:
        """Windows doesn't support Unix-style process group termination."""
        return False

    def kill_process_tree(self, pid: int) -> None:
        """Kill a process and all its children using taskkill."""
        try:
            # Use taskkill to kill process tree
            # Redirect output to DEVNULL to avoid hanging
            subprocess.run(
                ['taskkill', '/F', '/T', '/PID', str(pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5
            )
        except Exception as e:
            if self.verbose:
                print(f"Warning: Failed to kill process {pid}: {e}", file=sys.stderr)

    def kill_sintra_processes(self, test_names: List[str]) -> None:
        """Kill all existing sintra test processes by name."""
        try:
            for name in test_names:
                # Ensure .exe extension
                if not name.lower().endswith('.exe'):
                    name = f'{name}.exe'
                subprocess.run(
                    ['taskkill', '/F', '/IM', name],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=3
                )
        except Exception as e:
            if self.verbose:
                print(f"Warning: Failed to kill sintra processes: {e}", file=sys.stderr)

    def is_crash_exit(self, returncode: int) -> bool:
        """Return True if the exit code represents an abnormal termination on Windows."""
        if returncode == 0:
            return False
        # Windows crash codes are typically large unsigned values such as 0xC0000005
        return returncode < 0 or returncode >= 0xC0000000

    def decode_signal(self, returncode: int) -> Optional[int]:
        """Windows doesn't use Unix signals."""
        return None

    def supports_signals(self) -> bool:
        """Windows doesn't support Unix-style signals."""
        return False

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        """Windows doesn't have process groups in the Unix sense."""
        return []

    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Return all descendant process IDs for the provided root PID."""
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
                    print(
                        f"[DEBUG] collect_descendant_pids({root_pid}) found "
                        f"{len(descendants)} descendants via psutil: {descendants}",
                        file=sys.stderr
                    )
            except (_PSUTIL.NoSuchProcess, _PSUTIL.AccessDenied) as e:
                if self.verbose:
                    print(
                        f"[DEBUG] collect_descendant_pids({root_pid}) psutil failed: "
                        f"{e}, trying Win32_Process fallback",
                        file=sys.stderr
                    )
            except Exception as e:
                if self.verbose:
                    print(
                        f"[DEBUG] collect_descendant_pids({root_pid}) psutil unexpected "
                        f"error ({type(e).__name__}): {e}, trying Win32_Process fallback",
                        file=sys.stderr
                    )

        # Fallback: Query Win32_Process directly (works even if parent is dead)
        if not psutil_worked:
            descendants = self._collect_descendant_pids_windows_fallback(root_pid)
            if self.verbose and descendants:
                print(
                    f"[DEBUG] collect_descendant_pids({root_pid}) found "
                    f"{len(descendants)} descendants via Win32_Process: {descendants}",
                    file=sys.stderr
                )

        return descendants

    def _collect_descendant_pids_windows_fallback(self, root_pid: int) -> List[int]:
        """
        Fallback method that queries Win32_Process directly.
        Works even if the parent process has already exited, unlike psutil.children().
        """
        # Validate root_pid
        if not isinstance(root_pid, int):
            raise ValueError(f"root_pid must be an integer, got {type(root_pid).__name__}")

        if root_pid <= 0 or root_pid > 0xFFFFFFFF:  # Windows PIDs are 32-bit
            raise ValueError(f"root_pid out of valid range: {root_pid}")

        powershell_path = shutil.which("powershell")
        if not powershell_path:
            if self.verbose:
                print(
                    "[DEBUG] _collect_descendant_pids_windows_fallback: powershell not found",
                    file=sys.stderr
                )
            return []

        # PowerShell script to recursively find all descendants
        # Uses param($RootPid) to safely accept the PID as a parameter
        script = (
            "param($RootPid); "
            "function Get-ChildPids($ParentPid){"
            "  $children = Get-CimInstance Win32_Process -Filter \"ParentProcessId=$ParentPid\" -ErrorAction SilentlyContinue;"
            "  foreach($child in $children){"
            "    $child.ProcessId;"
            "    Get-ChildPids $child.ProcessId"
            "  }"
            "}"
            "; Get-ChildPids $RootPid"
        )

        try:
            result = subprocess.run(
                [
                    powershell_path,
                    "-NoProfile",
                    "-NonInteractive",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-Command",
                    script,
                    str(root_pid),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
            )

            if result.returncode != 0:
                if self.verbose:
                    print(
                        f"[DEBUG] _collect_descendant_pids_windows_fallback: "
                        f"PowerShell failed with code {result.returncode}",
                        file=sys.stderr
                    )
                return []

            pids: List[int] = []
            for line in result.stdout.strip().split('\n'):
                line = line.strip()
                if line and line.isdigit():
                    try:
                        pid = int(line)
                        if pid > 0:
                            pids.append(pid)
                    except ValueError:
                        continue

            return pids

        except (subprocess.SubprocessError, OSError) as e:
            if self.verbose:
                print(
                    f"[DEBUG] _collect_descendant_pids_windows_fallback: exception: {e}",
                    file=sys.stderr
                )
            return []

    def snapshot_process_table(self) -> List[tuple[int, int, int]]:
        """Windows doesn't support ps command."""
        return []
