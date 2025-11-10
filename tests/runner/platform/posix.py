from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

from ._psutil import load_psutil
from .base import ManualSignalSupport, PlatformSupport


_PSUTIL = load_psutil()


class PosixPlatformSupport(PlatformSupport):
    """Platform helpers for Unix-like systems."""

    def available_memory_bytes(self) -> Optional[int]:
        if _PSUTIL is not None:
            try:
                return int(_PSUTIL.virtual_memory().available)
            except Exception:
                pass

        try:
            page_size = os.sysconf("SC_PAGE_SIZE")
            avail_pages = os.sysconf("SC_AVPHYS_PAGES")
        except (AttributeError, OSError, ValueError):
            page_size = None
            avail_pages = None

        if isinstance(page_size, int) and isinstance(avail_pages, int):
            return page_size * avail_pages

        if sys.platform == "darwin":
            return self._available_memory_macos()

        return None

    def _available_memory_macos(self) -> Optional[int]:
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

    def core_dump_directories(self, base: Set[Path]) -> Set[Path]:
        if sys.platform == "darwin":
            base.add(Path("/cores"))
            base.add(Path.home() / "Library" / "Logs" / "DiagnosticReports")
        else:
            base.add(Path("/var/lib/systemd/coredump"))
        return base

    def kill_process_tree(self, pid: int) -> None:
        import signal

        try:
            pgid = os.getpgid(pid)
        except ProcessLookupError:
            pgid = None

        if pgid is not None:
            os.killpg(pgid, signal.SIGKILL)
        else:
            os.kill(pid, signal.SIGKILL)

    def adjust_executable_name(self, name: str) -> str:
        return name

    def configure_popen(self, popen_kwargs: Dict[str, object]) -> None:
        popen_kwargs.setdefault("start_new_session", True)

    def manual_signal_support(self) -> Optional[ManualSignalSupport]:
        try:
            import signal
        except ImportError:
            return None

        if not hasattr(signal, "SIGUSR1"):
            return None

        return ManualSignalSupport(module=signal, signal_number=signal.SIGUSR1)

    def collect_process_group_pids(self, pgid: int) -> List[int]:
        proc_path = Path("/proc")
        entries: Optional[List[Path]] = None
        if proc_path.exists():
            try:
                entries = list(proc_path.iterdir())
            except Exception:
                entries = None

        if entries is None:
            return self._collect_process_group_pids_via_ps(pgid)

        pids: List[int] = []
        for entry in entries:
            if not entry.is_dir():
                continue
            name = entry.name
            if not name.isdigit():
                continue
            try:
                candidate_pid = int(name)
            except ValueError:
                continue
            try:
                candidate_pgid = os.getpgid(candidate_pid)
            except (ProcessLookupError, PermissionError):
                continue
            if candidate_pgid == pgid:
                pids.append(candidate_pid)
        return pids

    def _collect_process_group_pids_via_ps(self, pgid: int) -> List[int]:
        ps_executable = shutil.which("ps")
        if not ps_executable:
            return []

        try:
            result = subprocess.run(
                [ps_executable, "-eo", "pid=,pgid="],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
                check=True,
            )
        except subprocess.SubprocessError:
            return []

        matches: List[int] = []
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 2:
                continue
            try:
                pid_value = int(parts[0])
                pgid_value = int(parts[1])
            except ValueError:
                continue
            if pgid_value == pgid:
                matches.append(pid_value)
        return matches

    def collect_descendant_pids(self, root_pid: int) -> List[int]:
        proc_path = Path("/proc")
        entries: Optional[List[Path]] = None
        if proc_path.exists():
            try:
                entries = list(proc_path.iterdir())
            except Exception:
                entries = None

        if entries is None:
            return self._collect_descendant_pids_via_ps(root_pid)

        parent_to_children: Dict[int, List[int]] = {}
        for entry in entries:
            if not entry.is_dir():
                continue
            name = entry.name
            if not name.isdigit():
                continue
            try:
                pid = int(name)
            except ValueError:
                continue
            stat_path = entry / "stat"
            try:
                stat_content = stat_path.read_text()
            except (OSError, UnicodeDecodeError):
                continue
            close_paren = stat_content.find(")")
            if close_paren == -1 or close_paren + 2 >= len(stat_content):
                continue
            remainder = stat_content[close_paren + 2 :].split()
            if len(remainder) < 2:
                continue
            try:
                parent_pid = int(remainder[1])
            except ValueError:
                continue
            parent_to_children.setdefault(parent_pid, []).append(pid)

        descendants: List[int] = []
        visited: Set[int] = set()
        stack: List[int] = parent_to_children.get(root_pid, [])[:]

        while stack:
            current = stack.pop()
            if current in visited or current == root_pid:
                continue
            visited.add(current)
            descendants.append(current)
            stack.extend(parent_to_children.get(current, []))

        return descendants

    def _collect_descendant_pids_via_ps(self, root_pid: int) -> List[int]:
        ps_executable = shutil.which("ps")
        if not ps_executable:
            return []

        try:
            result = subprocess.run(
                [ps_executable, "-eo", "pid=,ppid="],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
                check=True,
            )
        except subprocess.SubprocessError:
            return []

        parent_to_children: Dict[int, List[int]] = {}
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 2:
                continue
            try:
                pid_value = int(parts[0])
                ppid_value = int(parts[1])
            except ValueError:
                continue
            parent_to_children.setdefault(ppid_value, []).append(pid_value)

        descendants: List[int] = []
        visited: Set[int] = set()
        stack: List[int] = parent_to_children.get(root_pid, [])[:]

        while stack:
            current = stack.pop()
            if current in visited or current == root_pid:
                continue
            visited.add(current)
            descendants.append(current)
            stack.extend(parent_to_children.get(current, []))

        return descendants

    def describe_processes(self, pids: Sequence[int]) -> Dict[int, str]:
        unique_pids = sorted({pid for pid in pids if isinstance(pid, int) and pid > 0})
        if not unique_pids:
            return {}

        details: Dict[int, str] = {}

        if _PSUTIL is not None:
            psutil_module = _PSUTIL
            now = time.time()
            for pid in unique_pids:
                if pid in details:
                    continue
                try:
                    proc = psutil_module.Process(pid)
                except Exception:
                    continue

                parts: List[str] = []
                try:
                    parts.append(f"ppid={proc.ppid()}")
                except Exception:
                    pass
                try:
                    parts.append(f"pgid={os.getpgid(pid)}")
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
                    uptime = now - create_time
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

        remaining = [pid for pid in unique_pids if pid not in details]
        ps_executable = shutil.which("ps")
        if remaining and ps_executable:
            chunk_size = 16
            for offset in range(0, len(remaining), chunk_size):
                chunk = remaining[offset : offset + chunk_size]
                command = [
                    ps_executable,
                    "-o",
                    "pid=,ppid=,pgid=,stat=,etime=,command=",
                    "-p",
                    ",".join(str(pid) for pid in chunk),
                ]
                try:
                    result = subprocess.run(
                        command,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        check=True,
                        timeout=5,
                    )
                except subprocess.SubprocessError:
                    continue

                for line in result.stdout.splitlines():
                    line = line.strip()
                    if not line:
                        continue
                    fields = line.split(None, 5)
                    if len(fields) < 5:
                        continue
                    try:
                        pid_value = int(fields[0])
                    except ValueError:
                        continue
                    description_parts = [
                        f"ppid={fields[1]}",
                        f"pgid={fields[2]}",
                        f"stat={fields[3]}",
                        f"etime={fields[4]}",
                    ]
                    if len(fields) >= 6:
                        description_parts.append(f"cmd={fields[5]}")
                    details[pid_value] = " ".join(description_parts)

        for pid in unique_pids:
            details.setdefault(pid, "details unavailable")

        return details

    def snapshot_process_table(self) -> List[Tuple[int, int, int]]:
        ps_executable = shutil.which("ps")
        if not ps_executable:
            return []

        try:
            result = subprocess.run(
                [ps_executable, "-eo", "pid=,ppid=,pgid="],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
                check=True,
            )
        except subprocess.SubprocessError:
            return []

        snapshot: List[Tuple[int, int, int]] = []
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) != 3:
                continue
            try:
                pid_value = int(parts[0])
                ppid_value = int(parts[1])
                pgid_value = int(parts[2])
            except ValueError:
                continue
            snapshot.append((pid_value, ppid_value, pgid_value))
        return snapshot

    def kill_all_sintra_processes(self) -> None:
        try:
            subprocess.run(
                ["pkill", "-9", "sintra"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=5,
            )
        except Exception:
            pass
