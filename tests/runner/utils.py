from __future__ import annotations

import os
import shutil
from typing import Optional


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
