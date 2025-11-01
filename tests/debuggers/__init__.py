from __future__ import annotations

import os
import sys
from typing import Callable, Iterable, Optional

from .base import DebuggerStrategy, NullDebuggerStrategy


def get_debugger_strategy(
    verbose: bool,
    *,
    print_fn: Optional[Callable[[str], None]] = None,
    color: object = None,
    collect_process_group_pids: Optional[Callable[[int], Iterable[int]]] = None,
    collect_descendant_pids: Optional[Callable[[int], Iterable[int]]] = None,
) -> DebuggerStrategy:
    """Return the debugger strategy appropriate for the current platform."""

    common_kwargs = dict(
        print_fn=print_fn,
        color=color,
        collect_process_group_pids=collect_process_group_pids,
        collect_descendant_pids=collect_descendant_pids,
    )

    if sys.platform == "win32":
        from .windows import WindowsDebuggerStrategy

        return WindowsDebuggerStrategy(verbose, **common_kwargs)

    if os.name == "posix":
        from .unix import UnixDebuggerStrategy

        return UnixDebuggerStrategy(verbose, **common_kwargs)

    return NullDebuggerStrategy(verbose, **common_kwargs)


__all__ = ["DebuggerStrategy", "get_debugger_strategy"]
