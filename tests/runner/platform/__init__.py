from __future__ import annotations

import sys

from .base import ManualSignalSupport, PlatformSupport
from .posix import PosixPlatformSupport
from .windows import WindowsPlatformSupport


def get_platform_support(*, verbose: bool = False) -> PlatformSupport:
    """Return the platform adapter for the current host."""

    if sys.platform == "win32":
        return WindowsPlatformSupport(verbose=verbose)
    return PosixPlatformSupport(verbose=verbose)


__all__ = [
    "ManualSignalSupport",
    "PlatformSupport",
    "get_platform_support",
]
