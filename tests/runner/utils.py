from __future__ import annotations

import os
import shutil
import subprocess
from typing import List, Optional, Sequence, Tuple
from .platform._psutil import load_psutil

_PSUTIL = load_psutil()


class Color:
    """ANSI color codes for terminal output"""

    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


def format_duration(seconds: float) -> str:
    """Format duration in human-readable format."""

    if seconds < 1:
        return f"{seconds * 1000:.0f}ms"
    return f"{seconds:.2f}s"


def format_size(num_bytes: Optional[int]) -> str:
    """Return a human-friendly representation of ``num_bytes``."""

    if num_bytes is None or num_bytes < 0:
        return "unknown"

    units = ["B", "KB", "MB", "GB", "TB", "PB"]
    value = float(num_bytes)
    for unit in units:
        if value < 1024.0:
            return f"{value:.2f} {unit}"
        value /= 1024.0
    return f"{value:.2f} EB"


def available_disk_bytes(path: os.PathLike[str]) -> Optional[int]:
    """Return free disk space for ``path``."""

    try:
        usage = shutil.disk_usage(path)
    except Exception:
        return None
    return usage.free


def env_flag(name: str) -> bool:
    """Return True if the specified environment variable is truthy."""

    value = os.environ.get(name)
    if value is None:
        return False

    normalized = value.strip().lower()
    if not normalized:
        return False

    return normalized not in {"0", "false", "no", "off"}


def find_lingering_processes(prefixes: Sequence[str]) -> List[Tuple[int, str]]:
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
