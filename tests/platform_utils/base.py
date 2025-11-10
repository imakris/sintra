"""Base class for platform-specific utilities."""
from __future__ import annotations

import subprocess
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Any, Dict, List, Optional, Set


class PlatformUtils(ABC):
    """Abstract base class for platform-specific test runner utilities."""

    def __init__(self, verbose: bool = False):
        self.verbose = verbose

    @abstractmethod
    def get_available_memory_bytes(self) -> Optional[int]:
        """Return available memory in bytes, or None if unable to determine."""
        pass

    @abstractmethod
    def get_core_dump_search_directories(
        self,
        test_path: Path,
        build_dir: Path,
        scratch_base: Path,
        cwd: Path,
    ) -> List[Path]:
        """Return directories that may contain core dumps."""
        pass

    @abstractmethod
    def normalize_executable_name(self, name: str) -> str:
        """Normalize executable name (e.g., strip .exe on Windows)."""
        pass

    @abstractmethod
    def get_process_creation_kwargs(self, base_kwargs: Dict[str, Any]) -> Dict[str, Any]:
        """Return platform-specific kwargs for subprocess.Popen."""
        pass

    @abstractmethod
    def can_terminate_process_group(self) -> bool:
        """Return True if platform supports process group termination."""
        pass

    @abstractmethod
    def kill_process_tree(self, pid: int) -> None:
        """Kill a process and all its children."""
        pass

    @abstractmethod
    def kill_sintra_processes(self, test_names: List[str]) -> None:
        """Kill all existing sintra test processes."""
        pass

    @abstractmethod
    def is_crash_exit(self, returncode: int) -> bool:
        """Return True if the exit code represents an abnormal termination."""
        pass

    @abstractmethod
    def decode_signal(self, returncode: int) -> Optional[int]:
        """Return the signal number encoded in a return code, or None."""
        pass

    @abstractmethod
    def supports_signals(self) -> bool:
        """Return True if platform supports Unix-style signals."""
        pass

    @abstractmethod
    def collect_process_group_pids(self, pgid: int) -> List[int]:
        """Return all process IDs belonging to the provided process group."""
        pass

    @abstractmethod
    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        """Return all descendant process IDs for the provided root PID."""
        pass

    @abstractmethod
    def snapshot_process_table(self) -> List[tuple[int, int, int]]:
        """Return (pid, ppid, pgid) tuples for all processes."""
        pass


class NullPlatformUtils(PlatformUtils):
    """Null implementation that provides safe defaults."""

    def get_available_memory_bytes(self) -> Optional[int]:
        return None

    def get_core_dump_search_directories(
        self,
        test_path: Path,
        build_dir: Path,
        scratch_base: Path,
        cwd: Path,
    ) -> List[Path]:
        candidates: Set[Path] = {
            test_path.parent.resolve(),
            cwd.resolve(),
            build_dir.resolve(),
            scratch_base,
        }
        return [path for path in candidates if path]

    def normalize_executable_name(self, name: str) -> str:
        return name

    def get_process_creation_kwargs(self, base_kwargs: Dict[str, Any]) -> Dict[str, Any]:
        return base_kwargs

    def can_terminate_process_group(self) -> bool:
        return False

    def kill_process_tree(self, pid: int) -> None:
        pass

    def kill_sintra_processes(self, test_names: List[str]) -> None:
        pass

    def is_crash_exit(self, returncode: int) -> bool:
        return returncode != 0

    def decode_signal(self, returncode: int) -> Optional[int]:
        return None

    def supports_signals(self) -> bool:
        return False

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        return []

    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        return []

    def snapshot_process_table(self) -> List[tuple[int, int, int]]:
        return []
