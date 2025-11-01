from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Any, Callable, Iterable, Optional, Tuple, TYPE_CHECKING

if TYPE_CHECKING:
    from tests.run_tests import TestInvocation


class _NullColor:
    BLUE = ""
    GREEN = ""
    RED = ""
    YELLOW = ""
    BOLD = ""
    RESET = ""


class DebuggerStrategy(ABC):
    """Abstract strategy used to capture debugger information per-platform."""

    def __init__(
        self,
        verbose: bool,
        *,
        print_fn: Optional[Callable[[str], None]] = None,
        color: Any = None,
        collect_process_group_pids: Optional[Callable[[int], Iterable[int]]] = None,
        collect_descendant_pids: Optional[Callable[[int], Iterable[int]]] = None,
    ) -> None:
        self.verbose = verbose
        self._print = print_fn
        self._color = color if color is not None else _NullColor()
        self._collect_process_group_pids = collect_process_group_pids
        self._collect_descendant_pids = collect_descendant_pids

    def prepare(self) -> None:
        """Perform any upfront preparation before the first test runs."""

    def ensure_crash_dumps(self) -> Optional[str]:
        """Return an error string if crash dumps cannot be configured."""
        return None

    def configure_jit_debugging(self) -> Optional[str]:
        """Return an error string if JIT debugging could not be configured."""
        return None

    @abstractmethod
    def capture_process_stacks(
        self,
        pid: int,
        process_group: Optional[int] = None,
    ) -> Tuple[str, str]:
        """Capture stacks for a running process tree."""

    @abstractmethod
    def capture_core_dump_stack(
        self,
        invocation: "TestInvocation",
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        """Capture stacks from a post-mortem artifact for the process."""

    def _log(self, message: str) -> None:
        if self._print is not None:
            self._print(message)


class NullDebuggerStrategy(DebuggerStrategy):
    """Fallback strategy used when the platform lacks debugger support."""

    def capture_process_stacks(
        self,
        pid: int,
        process_group: Optional[int] = None,
    ) -> Tuple[str, str]:
        return "", "stack capture unavailable on this platform"

    def capture_core_dump_stack(
        self,
        invocation: "TestInvocation",
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        return "", "post-mortem stack capture unavailable on this platform"
