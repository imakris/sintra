from __future__ import annotations

import shutil
import subprocess
import sys
import time
from typing import Dict, List, Optional, Sequence

from ._psutil import load_psutil
from .base import PlatformSupport


_PSUTIL = load_psutil()


class WindowsPlatformSupport(PlatformSupport):
    """Platform helpers for Windows hosts."""

    @property
    def is_windows(self) -> bool:
        return True

    def adjust_executable_name(self, name: str) -> str:
        lowered = name.lower()
        if lowered.endswith(".exe"):
            return name[:-4]
        return name

    def configure_popen(self, popen_kwargs: Dict[str, object]) -> None:
        creationflags = 0
        if hasattr(subprocess, "CREATE_NEW_PROCESS_GROUP"):
            creationflags = subprocess.CREATE_NEW_PROCESS_GROUP
        popen_kwargs["creationflags"] = creationflags

    def available_memory_bytes(self) -> Optional[int]:
        if _PSUTIL is None:
            return None
        try:
            return int(_PSUTIL.virtual_memory().available)
        except Exception:
            return None

    def kill_process_tree(self, pid: int) -> None:
        subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(pid)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=5,
        )

    def kill_all_sintra_processes(self) -> None:
        # Broad kill to ensure any lingering test process (including dynamically spawned ones)
        # is terminated before/after runs. Wildcard handling is done by taskkill.
        patterns = [
            "sintra_*.exe",
            "join_swarm_midflight_test*.exe",
        ]
        for pattern in patterns:
            subprocess.run(
                ["taskkill", "/F", "/T", "/IM", pattern],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        return []

    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        descendants: List[int] = []
        psutil_worked = False

        if _PSUTIL is not None:
            try:
                root_proc = _PSUTIL.Process(root_pid)
                children = root_proc.children(recursive=True)
                descendants = [child.pid for child in children]
                psutil_worked = True
                if self.verbose:
                    print(
                        f"[DEBUG] _collect_descendant_pids({root_pid}) found {len(descendants)} descendants via psutil: {descendants}",
                        file=sys.stderr,
                    )
            except (_PSUTIL.NoSuchProcess, _PSUTIL.AccessDenied) as exc:  # type: ignore[attr-defined]
                if self.verbose:
                    print(
                        f"[DEBUG] _collect_descendant_pids({root_pid}) psutil failed: {exc}, trying Win32_Process fallback",
                        file=sys.stderr,
                    )
            except Exception as exc:
                if self.verbose:
                    print(
                        f"[DEBUG] _collect_descendant_pids({root_pid}) psutil unexpected error ({type(exc).__name__}): {exc}, trying Win32_Process fallback",
                        file=sys.stderr,
                    )

        if psutil_worked:
            return descendants

        fallback = self._collect_descendant_pids_windows_fallback(root_pid)
        if self.verbose and fallback:
            print(
                f"[DEBUG] _collect_descendant_pids({root_pid}) found {len(fallback)} descendants via Win32_Process: {fallback}",
                file=sys.stderr,
            )
        return fallback

    def _collect_descendant_pids_windows_fallback(self, root_pid: int) -> List[int]:
        if not isinstance(root_pid, int):
            if self.verbose:
                print(
                    f"[DEBUG] _collect_descendant_pids_windows_fallback: invalid root_pid type: {type(root_pid)}",
                    file=sys.stderr,
                )
            return []
        if root_pid <= 0 or root_pid > 0xFFFFFFFF:
            if self.verbose:
                print(
                    f"[DEBUG] _collect_descendant_pids_windows_fallback: root_pid out of range: {root_pid}",
                    file=sys.stderr,
                )
            return []

        powershell_path = shutil.which("powershell")
        if not powershell_path:
            if self.verbose:
                print(
                    "[DEBUG] _collect_descendant_pids_windows_fallback: powershell not found",
                    file=sys.stderr,
                )
            return []

        script = (
            "function Get-ChildPids($ParentPid){"
            "  $children = Get-CimInstance Win32_Process -Filter \"ParentProcessId=$ParentPid\" -ErrorAction SilentlyContinue;"
            "  foreach($child in $children){"
            "    $child.ProcessId;"
            "    Get-ChildPids $child.ProcessId"
            "  }"
            "}"
            f"; Get-ChildPids {root_pid}"
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
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
            )
        except (subprocess.SubprocessError, OSError) as exc:
            if self.verbose:
                print(
                    f"[DEBUG] _collect_descendant_pids_windows_fallback: exception: {exc}",
                    file=sys.stderr,
                )
            return []

        if result.returncode != 0:
            if self.verbose:
                print(
                    f"[DEBUG] _collect_descendant_pids_windows_fallback: PowerShell failed with code {result.returncode}",
                    file=sys.stderr,
                )
            return []

        pids: List[int] = []
        for line in result.stdout.strip().split("\n"):
            text = line.strip()
            if text and text.isdigit():
                try:
                    pid = int(text)
                except ValueError:
                    continue
                if pid > 0:
                    pids.append(pid)
        return pids

    def describe_processes(self, pids: Sequence[int]) -> Dict[int, str]:
        unique = sorted({pid for pid in pids if isinstance(pid, int) and pid > 0})
        if not unique or _PSUTIL is None:
            return {}

        details: Dict[int, str] = {}

        for pid in unique:
            try:
                proc = _PSUTIL.Process(pid)
            except Exception:
                continue

            parts: List[str] = []
            try:
                parts.append(f"ppid={proc.ppid()}")
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
                uptime = time.time() - create_time
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

        return details
