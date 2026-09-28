from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .base import DebuggerStrategy

WINDOWS_DEBUGGER_CACHE_ENV = "SINTRA_WINDOWS_DEBUGGER_CACHE"
WINDOWS_SYMBOL_PATH_ENV = "SINTRA_WINDOWS_SYMBOL_PATH"


class WindowsDebuggerStrategy(DebuggerStrategy):
    """Debugger strategy for Windows hosts."""

    _WINDOWS_DEBUGGER_SUCCESS_CODES = {0x00000000, 0xD000010A}

    def __init__(self, verbose: bool, **kwargs) -> None:
        super().__init__(verbose, **kwargs)
        self._debugger_cache: Dict[str, Tuple[Optional[str], str]] = {}

    # Interface methods -------------------------------------------------
    def prepare(self) -> None:
        debugger, path, error = self._resolve_windows_debugger()
        if path and self.verbose:
            self._log(
                f"{self._color.BLUE}Using Windows debugger '{debugger}' at {path}{self._color.RESET}"
            )
        elif error:
            self._log(
                f"{self._color.YELLOW}Warning: {error}. Stack capture may be unavailable.{self._color.RESET}"
            )

    def ensure_crash_dumps(self) -> Optional[str]:
        return (
            "Windows post-mortem dumps require operator-configured WER LocalDumps; "
            "the runner does not enable dump collection. An installed debugger alone "
            "only supports capture while the process is still alive"
        )

    def capture_process_stacks(
        self,
        pid: int,
        process_group: Optional[int] = None,
    ) -> Tuple[str, str]:
        return self._capture_process_stacks_windows(pid)

    def capture_core_dump_stack(
        self,
        invocation: "TestInvocation",
        start_time: float,
        pid: int,
        working_dir: Path,
    ) -> Tuple[str, str]:
        return self._capture_windows_crash_dump(invocation, start_time, pid, working_dir)

    # Discovery never installs tools or changes machine configuration.
    def _locate_windows_debugger(self, executable: str) -> Tuple[Optional[str], str]:
        cache_key = executable.lower()
        if cache_key in self._debugger_cache:
            return self._debugger_cache[cache_key]

        path = shutil.which(executable)
        if not path:
            roots = [
                Path(value) / "Windows Kits" / "10" / "Debuggers"
                for name in ("ProgramFiles(x86)", "ProgramFiles")
                if (value := os.environ.get(name))
            ]
            roots.append(self._get_windows_debugger_cache_dir() /
                         "winsdk_debuggers" / "Windows Kits" / "10" / "Debuggers")
            names = [executable] if executable.endswith(".exe") else [executable + ".exe"]
            located = self._find_debugger_executable(roots, names)
            if located:
                path = str(located)

        result = (path, "" if path else
                  f"{executable} unavailable; install Windows Debugging Tools or add it to PATH")
        self._debugger_cache[cache_key] = result
        return result

    def _symbol_path_command(self) -> str:
        """Return the debugger command prefix that configures symbol paths.

        By default we call `.symfix` to enable the Microsoft public symbol
        server, and then append any user-provided local symbol search path
        from SINTRA_WINDOWS_SYMBOL_PATH. This allows CI to point cdb/windbg at
        the build directory containing PDBs for Sintra binaries while still
        keeping OS symbols available.
        """
        extra = os.environ.get(WINDOWS_SYMBOL_PATH_ENV, "").strip()
        if not extra:
            return ".symfix; .reload"

        # Normalize separators and deduplicate empty components
        parts = [p for p in extra.replace("|", ";").split(";") if p]
        if not parts:
            return ".symfix; .reload"

        symbol_path = ";".join(parts)
        # Use a single quoted argument so embedded semicolons are preserved as
        # path separators by the debugger.
        return f'.symfix; .sympath+ "{symbol_path}"; .reload'

    def _get_windows_debugger_cache_dir(self) -> Path:
        override = os.environ.get(WINDOWS_DEBUGGER_CACHE_ENV)
        if override:
            return Path(override)

        local_app_data = os.environ.get("LOCALAPPDATA")
        if local_app_data:
            base_dir = Path(local_app_data)
        else:
            base_dir = Path.home() / "AppData" / "Local"

        return base_dir / "sintra" / "debugger_cache"

    def _find_debugger_executable(
        self,
        debugger_roots: List[Path],
        executable_names: List[str],
    ) -> Optional[Path]:
        # Prefer native x64 tools across every installation before accepting
        # a fallback from one root (which may contain only an x86 debugger).
        candidate_dirs = [
            root / relative
            for root in debugger_roots
            for relative in ("x64", "amd64", "dbg/amd64", "bin/x64")
        ]
        candidate_dirs.extend(debugger_roots)

        for directory in candidate_dirs:
            try:
                if not directory.exists():
                    continue
            except OSError:
                continue

            for name in executable_names:
                candidate = directory / name
                try:
                    if candidate.exists():
                        return candidate
                except OSError:
                    continue

        for root in debugger_roots:
            for name in executable_names:
                try:
                    matches = list(root.rglob(name))
                except OSError:
                    matches = []
                if matches:
                    return matches[0]

        return None

    # Debugger resolution ------------------------------------------------
    def _resolve_windows_debugger(self) -> Tuple[Optional[str], Optional[str], str]:
        debugger_candidates = ["cdb", "ntsd", "windbg"]
        errors: List[str] = []

        for debugger in debugger_candidates:
            path, error = self._locate_windows_debugger(debugger)
            if path:
                return debugger, path, ""
            if error:
                errors.append(f"{debugger}: {error}")

        if errors:
            return None, None, "; ".join(errors)

        return None, None, "no Windows debugger available"

    # Stack capture helpers ---------------------------------------------
    def _configured_dump_directories(self, executable: str) -> List[Path]:
        """Read operator-provided WER destinations without enabling dumps."""
        if sys.platform != "win32":
            return []
        import winreg

        subkey = r"Software\Microsoft\Windows\Windows Error Reporting\LocalDumps"
        directories = []
        for root in (winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE):
            for key_name in (subkey + "\\" + executable, subkey):
                try:
                    with winreg.OpenKey(root, key_name, 0, winreg.KEY_READ) as key:
                        value, _ = winreg.QueryValueEx(key, "DumpFolder")
                    directories.append(Path(os.path.expandvars(value)))
                except OSError:
                    continue
        return directories

    def _capture_windows_crash_dump(
        self,
        invocation: "TestInvocation",
        start_time: float,
        pid: int,
        working_dir: Path,
    ) -> Tuple[str, str]:
        debugger_name, debugger_path, debugger_error = self._resolve_windows_debugger()
        if not debugger_path:
            return "", debugger_error

        candidate_dirs = self._configured_dump_directories(invocation.path.name)
        candidate_dirs.append(working_dir)

        local_app_data = os.environ.get("LOCALAPPDATA")
        if local_app_data:
            candidate_dirs.append(Path(local_app_data) / "CrashDumps")
        else:
            candidate_dirs.append(Path.home() / "AppData" / "Local" / "CrashDumps")

        exe_name_lower = invocation.path.name.lower()
        exe_stem_lower = invocation.path.stem.lower()
        pid_pattern = re.compile(rf"(?<!\d){pid}(?!\d)")

        candidate_dumps: List[Tuple[float, Path]] = []

        for directory in candidate_dirs:
            try:
                if not directory or not directory.exists():
                    continue
                entries = list(directory.iterdir())
            except OSError:
                continue

            for entry in entries:
                if not entry.is_file():
                    continue

                name_lower = entry.name.lower()
                if not name_lower.endswith(".dmp"):
                    continue

                if exe_name_lower not in name_lower and exe_stem_lower not in name_lower:
                    continue
                if not pid_pattern.search(name_lower):
                    continue

                try:
                    stat_info = entry.stat()
                except OSError:
                    continue

                if stat_info.st_mtime + 0.001 < start_time:
                    continue

                candidate_dumps.append((stat_info.st_mtime, entry))

        if not candidate_dumps:
            return "", (
                "no recent dump identifying the failed process was found; configure WER "
                "LocalDumps before running tests, or launch the failing test under an "
                "installed Windows debugger. Live minidumps cannot capture an exited process"
            )

        candidate_dumps.sort(key=lambda item: item[0], reverse=True)

        capture_errors: List[str] = []
        sym_cmd = self._symbol_path_command()
        for _, dump_path in candidate_dumps:
            command = [debugger_path]
            if debugger_name == "windbg":
                command.append("-Q")
            command.extend(["-z", str(dump_path), "-c", f"{sym_cmd}; ~* kP; qd"])
            try:
                result = subprocess.run(
                    command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True, timeout=120,
                )
            except (subprocess.SubprocessError, OSError) as exc:
                capture_errors.append(f"{dump_path}: {debugger_name} failed ({exc})")
                continue
            trace, error = self._read_stack_result(result)
            if trace:
                return f"{dump_path}\n{trace}", ""
            capture_errors.append(f"{dump_path}: {error}")
        return "", "; ".join(capture_errors)

    @classmethod
    def _read_stack_result(cls, result) -> Tuple[str, str]:
        """Accept debugger output only when it contains actual kP frame records."""
        output = result.stdout.strip()
        detail = "\n".join(part for part in (output, result.stderr.strip()) if part)
        code = cls._normalize_windows_returncode(result.returncode)
        fatal_messages = (
            "unable to examine process id", "could not open dump file",
            "debuggee initialization failed",
        )
        failed = any(message in detail.lower() for message in fatal_messages)
        # kP prints stack pointer, return address, then call site. Frame numbers
        # are optional; accept both x86 addresses and backtick-separated x64.
        address = r"[0-9a-fA-F]{8,16}(?:`[0-9a-fA-F]{8})?"
        frame = rf"(?m)^\s*(?:[0-9a-fA-F]{{2}}\s+)?{address}\s+{address}\s+\S+"
        header = re.search(r"(?m)^\s*(?:Child-SP|ChildEBP)\s+RetAddr[^\n]*Call Site\s*$", output)
        has_frames = header is not None and re.search(frame, output[header.end():]) is not None
        if not failed and code in cls._WINDOWS_DEBUGGER_SUCCESS_CODES and has_frames:
            return output, ""
        return "", f"debugger produced no usable stack (exit {cls._format_windows_returncode(code)}): {detail}"

    def _capture_process_stacks_windows(self, pid: int) -> Tuple[str, str]:
        debugger_name, debugger_path, debugger_error = self._resolve_windows_debugger()
        if not debugger_path:
            return "", debugger_error

        sym_cmd = self._symbol_path_command()

        target_pids = [pid]
        # Use the collect_descendant_pids callback (which has psutil support) if available,
        # otherwise fall back to the PowerShell-based method
        if self._collect_descendant_pids:
            child_pids = list(self._collect_descendant_pids(pid))
            target_pids.extend(child_pids)
            if self.verbose:
                print(f"[DEBUG] Windows debugger: root PID {pid}, children from callback: {child_pids}, total PIDs: {target_pids}", file=sys.stderr)
        else:
            child_pids = self._collect_windows_process_tree_pids(pid)
            target_pids.extend(child_pids)
            if self.verbose:
                print(f"[DEBUG] Windows debugger: root PID {pid}, children from PowerShell: {child_pids}, total PIDs: {target_pids}", file=sys.stderr)

        stack_outputs: List[str] = []
        capture_errors: List[str] = []
        for target_pid in sorted(set(target_pids)):
            command = [debugger_path]
            if debugger_name == "windbg":
                command.append("-Q")
            command.extend(["-pv", "-p", str(target_pid), "-c", f"{sym_cmd}; ~* kP; qd"])
            try:
                result = subprocess.run(
                    command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    text=True, timeout=60,
                )
            except (subprocess.SubprocessError, OSError) as exc:
                capture_errors.append(f"PID {target_pid}: {debugger_name} failed ({exc})")
                continue
            trace, error = self._read_stack_result(result)
            if trace:
                stack_outputs.append(f"PID {target_pid}\n{trace}")
                continue
            capture_errors.append(f"PID {target_pid}: {error}")
            if self._should_use_minidump_fallback(
                result.stdout, result.stderr, self._normalize_windows_returncode(result.returncode)
            ):
                trace, error = self._capture_stack_via_minidump(debugger_name, debugger_path, target_pid)
                if trace:
                    stack_outputs.append(f"PID {target_pid} (minidump)\n{trace}")
                elif error:
                    capture_errors.append(f"PID {target_pid}: {error}")
        return "\n\n".join(stack_outputs), "; ".join(capture_errors)

    def _capture_stack_via_minidump(
        self,
        debugger_name: str,
        debugger_path: str,
        pid: int,
    ) -> Tuple[str, Optional[str]]:
        """Create a minidump via comsvcs.dll and analyze it with the debugger."""

        dump_path = None
        try:
            dump_path, dump_error = self._create_minidump(pid)
            if dump_error:
                return "", dump_error
            if not dump_path:
                return "", "failed to create minidump"

            sym_cmd = self._symbol_path_command()

            command = [debugger_path]
            if debugger_name == "windbg":
                command.append("-Q")
            command.extend(["-z", str(dump_path), "-c", f"{sym_cmd}; ~* kP; qd"])

            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=120,
            )
        except (subprocess.SubprocessError, OSError) as exc:
            return "", f"minidump analysis failed: {exc}"
        finally:
            if dump_path:
                try:
                    os.remove(dump_path)
                except OSError:
                    pass

        return self._read_stack_result(result)

    def _create_minidump(self, pid: int) -> Tuple[Optional[str], Optional[str]]:
        """Generate a minidump for the given PID using comsvcs.dll."""

        system32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
        rundll32 = system32 / "rundll32.exe"
        if not rundll32.exists():
            return None, "rundll32.exe not found for minidump creation"

        try:
            tmp_fd, tmp_path = tempfile.mkstemp(prefix=f"sintra_{pid}_", suffix=".dmp")
            os.close(tmp_fd)
            # MiniDump creates the dump with CREATE_NEW, so keep only the unique name.
            os.remove(tmp_path)
        except OSError as exc:
            return None, f"failed to allocate dump file: {exc}"
        # MiniDump splits its arguments at spaces and rejects quoted paths.
        if " " in tmp_path:
            return None, (
                f"minidump path '{tmp_path}' contains a space, which comsvcs MiniDump "
                "cannot parse; point TMP at a directory without spaces"
            )

        # rundll32 parses its own command line. Quotes that subprocess would add to a
        # list item make rundll32 treat "dll, entry" as a module name and report the
        # load failure in a modal dialog, so the line is passed verbatim.
        command = f"{rundll32} {system32 / 'comsvcs.dll'},MiniDump {pid} {tmp_path} full"
        try:
            # subprocess.run kills rundll32 on timeout, so a dialog cannot hold the runner.
            result = subprocess.run(
                command,
                executable=str(rundll32),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=60,
            )
            code = self._normalize_windows_returncode(result.returncode)
            if code != 0 or not os.path.isfile(tmp_path) or not os.path.getsize(tmp_path):
                error = (
                    f"minidump of PID {pid} wrote no dump (exit {self._format_windows_returncode(code)}); "
                    "processes of other users require SeDebugPrivilege"
                )
            else:
                # MiniDump grants the dump to SYSTEM and Administrators only. Inherit the
                # temp directory's ACL so the debugger, running as this user, can read it.
                reset = subprocess.run(
                    [str(system32 / "icacls.exe"), tmp_path, "/reset", "/Q"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=30,
                )
                if reset.returncode == 0:
                    return tmp_path, None
                error = f"minidump ACL reset exited with {reset.returncode}; the dump is unreadable"
        except (subprocess.SubprocessError, OSError) as exc:
            error = f"minidump command failed: {exc}"

        try:
            os.remove(tmp_path)
        except OSError:
            pass
        return None, error

    @staticmethod
    def _should_use_minidump_fallback(
        output: str,
        stderr: str,
        returncode: int,
    ) -> bool:
        if not output:
            return True
        lowered = output.lower()
        if "unable to examine process" in lowered:
            return True
        if "hresult 0x80004002" in lowered:
            return True
        if "not attached as a debuggee" in lowered:
            return True
        lowered_err = stderr.lower()
        if "unable to examine process" in lowered_err or "hresult 0x80004002" in lowered_err:
            return True
        return False

    @staticmethod
    def _normalize_windows_returncode(returncode: int) -> int:
        return returncode & 0xFFFFFFFF

    @staticmethod
    def _format_windows_returncode(returncode: int) -> str:
        return f"0x{returncode:08X}"

    def _collect_windows_process_tree_pids(self, pid: int) -> List[int]:
        powershell_path = shutil.which("powershell")
        if not powershell_path:
            return []

        script = (
            "function Get-ChildPids($Pid){"
            "  $children = Get-CimInstance Win32_Process -Filter \"ParentProcessId=$Pid\";"
            "  foreach($child in $children){"
            "    $child.ProcessId;"
            "    Get-ChildPids $child.ProcessId"
            "  }"
            "}"
            f"; Get-ChildPids {pid}"
        )

        try:
            result = subprocess.run(
                [
                    powershell_path,
                    "-NoProfile",
                    "-Command",
                    script,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=30,
            )
        except (subprocess.SubprocessError, OSError):
            return []

        if result.returncode != 0:
            return []

        descendants: List[int] = []
        for line in result.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                descendants.append(int(line))
            except ValueError:
                continue

        return descendants
