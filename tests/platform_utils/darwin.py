"""Darwin (macOS)-specific platform utilities."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Set

from .unix import UnixPlatformUtils


class DarwinPlatformUtils(UnixPlatformUtils):
    """Darwin/macOS-specific implementations of platform utilities."""

    def get_available_memory_bytes(self) -> Optional[int]:
        """Return available memory in bytes using vm_stat on macOS."""
        # Try parent implementation first (psutil or sysconf)
        result = super().get_available_memory_bytes()
        if result is not None:
            return result

        # Darwin-specific: use vm_stat
        try:
            vm_stat = subprocess.run(
                ["vm_stat"],
                check=True,
                capture_output=True,
                text=True,
            )
        except Exception:
            return None

        page_size_bytes = 4096
        for line in vm_stat.stdout.splitlines():
            if line.startswith("page size of"):
                parts = line.split()
                try:
                    page_size_bytes = int(parts[3])
                except (IndexError, ValueError):
                    page_size_bytes = 4096
                break

        free_pages = 0
        inactive_pages = 0
        speculative_pages = 0

        for line in vm_stat.stdout.splitlines():
            if line.startswith("Pages free"):
                free_pages = int(line.split(":")[1].strip().strip("."))
            elif line.startswith("Pages inactive"):
                inactive_pages = int(line.split(":")[1].strip().strip("."))
            elif line.startswith("Pages speculative"):
                speculative_pages = int(line.split(":")[1].strip().strip("."))

        return page_size_bytes * (free_pages + inactive_pages + speculative_pages)

    def get_core_dump_search_directories(
        self,
        test_path: Path,
        build_dir: Path,
        scratch_base: Path,
        cwd: Path,
    ) -> List[Path]:
        """Return directories that may contain core dumps on macOS."""
        candidates: Set[Path] = {
            test_path.parent.resolve(),
            cwd.resolve(),
            build_dir.resolve(),
            scratch_base,
        }

        # Add Darwin-specific core dump locations
        candidates.add(Path("/cores"))
        candidates.add(Path.home() / "Library" / "Logs" / "DiagnosticReports")

        return [path for path in candidates if path]
