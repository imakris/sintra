from __future__ import annotations

import subprocess
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

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        return []

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
