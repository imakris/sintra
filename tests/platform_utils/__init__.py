"""Platform-specific utilities for test runner operations."""
from __future__ import annotations

import sys
from typing import Optional

from .base import PlatformUtils, NullPlatformUtils


def get_platform_utils(verbose: bool = False) -> PlatformUtils:
    """Return the platform utilities appropriate for the current platform."""

    if sys.platform == "win32":
        from .windows import WindowsPlatformUtils
        return WindowsPlatformUtils(verbose)

    if sys.platform == "darwin":
        from .darwin import DarwinPlatformUtils
        return DarwinPlatformUtils(verbose)

    if sys.platform.startswith("linux"):
        from .unix import UnixPlatformUtils
        return UnixPlatformUtils(verbose)

    # Fallback for other Unix-like systems
    from .unix import UnixPlatformUtils
    return UnixPlatformUtils(verbose)


__all__ = ["PlatformUtils", "get_platform_utils"]
