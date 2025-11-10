from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Set


@dataclass(frozen=True)
class ManualSignalSupport:
    """Information about a manually triggerable stack capture signal."""

    module: Any
    signal_number: int


class PlatformSupport:
    """Abstract base class describing platform specific behaviour."""

    def __init__(self, *, verbose: bool = False) -> None:
        self.verbose = verbose

    def adjust_executable_name(self, name: str) -> str:
        """Return ``name`` without platform specific executable suffixes."""

        return name

    @property
    def is_windows(self) -> bool:
        """Return True when running on Windows."""

        return False

    def configure_popen(self, popen_kwargs: Dict[str, Any]) -> None:
        """Mutate ``popen_kwargs`` with platform specific settings."""

        popen_kwargs.setdefault("start_new_session", True)

    def available_memory_bytes(self) -> Optional[int]:
        """Return available physical memory in bytes, if detectable."""

        return None

    def core_dump_directories(self, base: Set[Path]) -> Set[Path]:
        """Return candidate directories for core dumps."""

        return base

    def kill_process_tree(self, pid: int) -> None:
        """Terminate ``pid`` and its children."""

        raise NotImplementedError

    def kill_all_sintra_processes(self) -> None:
        """Best effort termination of lingering Sintra test binaries."""

        # Most platforms do not need additional work.

    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Return all descendant process IDs for ``root_pid``."""

        return []

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        """Return all process IDs belonging to ``pgid``."""

        return []

    def manual_signal_support(self) -> Optional[ManualSignalSupport]:
        """Return signal information for manual stack capture, if available."""

        return None

    def describe_processes(self, pids: Sequence[int]) -> Dict[int, str]:
        """Return a mapping of process IDs to human readable descriptions."""

        return {}

    def snapshot_process_table(self) -> List[tuple[int, int, int]]:
        """Return ``(pid, ppid, pgid)`` tuples describing the process table."""

        return []
