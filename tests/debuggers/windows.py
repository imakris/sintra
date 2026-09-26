from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

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
    ) -> Tuple[str, str]:
        return self._capture_windows_crash_dump(invocation, start_time, pid)

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
            for root in roots:
                located = self._find_debugger_executable(root, names)
                if located:
                    path = str(located)
                    break

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
        debugger_root: Path,
        executable_names: List[str],
    ) -> Optional[Path]:
        candidate_dirs = [
            debugger_root / "x64",
            debugger_root / "amd64",
            debugger_root / "dbg" / "amd64",
            debugger_root / "bin" / "x64",
            debugger_root,
        ]

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

        for name in executable_names:
            try:
                matches = list(debugger_root.rglob(name))
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
    ) -> Tuple[str, str]:
        debugger_name, debugger_path, debugger_error = self._resolve_windows_debugger()
        if not debugger_path:
            return "", debugger_error

        candidate_dirs = self._configured_dump_directories(invocation.path.name)
        candidate_dirs.extend([invocation.path.parent, Path.cwd()])

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
            return "", "no recent crash dump found"

        candidate_dumps.sort(key=lambda item: item[0], reverse=True)

        stack_outputs: List[str] = []
        capture_errors: List[str] = []
        fallback_outputs: List[Tuple[str, str, int, str]] = []

        sym_cmd = self._symbol_path_command()

        for _, dump_path in candidate_dumps:
            try:
                command = [debugger_path]
                if debugger_name == "windbg":
                    command.append("-Q")
                # Use kP to show stack with parameters (function arguments)
                command.extend(["-z", str(dump_path), "-c", f"{sym_cmd}; ~* kP; qd"])

                result = subprocess.run(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=120,
                )
            except (subprocess.SubprocessError, OSError) as exc:
                capture_errors.append(f"{dump_path}: {debugger_name} failed ({exc})")
                continue

            stdout = result.stdout.strip()
            stderr = result.stderr.strip()

            output = stdout
            output_from_stderr = False
            if not output and stderr:
                output = stderr
                output_from_stderr = True

            normalized_code = self._normalize_windows_returncode(result.returncode)
            exit_ok = normalized_code in self._WINDOWS_DEBUGGER_SUCCESS_CODES

            if exit_ok:
                if output:
                    note = ""
                    if normalized_code != 0:
                        note = (
                            f"\n\n[Debugger exited with code {self._format_windows_returncode(normalized_code)};"
                            " treated as success]"
                        )
                    stack_outputs.append(f"{dump_path}\n{output}{note}")
                continue

            if output:
                if not output_from_stderr:
                    fallback_outputs.append((str(dump_path), output, normalized_code, stderr))
                else:
                    capture_errors.append(
                        self._format_windows_debugger_failure(
                            debugger_name,
                            str(dump_path),
                            normalized_code,
                            stderr,
                        )
                    )
            else:
                capture_errors.append(
                    self._format_windows_debugger_failure(
                        debugger_name,
                        str(dump_path),
                        normalized_code,
                        stderr,
                    )
                )

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if fallback_outputs:
            annotated = []
            for label, output, normalized_code, stderr in fallback_outputs:
                detail = f"; stderr: {stderr}" if stderr else ""
                annotated.append(
                    f"{label}\n{output}\n\n[Debugger exited with code {self._format_windows_returncode(normalized_code)}; output may be incomplete{detail}]"
                )
            return "\n\n".join(annotated), "; ".join(capture_errors) if capture_errors else ""

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no stack data captured"

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
        fallback_outputs: List[Tuple[str, str, int, str]] = []

        analyzed_pids: Set[int] = set()

        for target_pid in sorted(set(target_pids)):
            if target_pid in analyzed_pids:
                continue
            analyzed_pids.add(target_pid)
            try:
                command = [debugger_path]
                if debugger_name == "windbg":
                    command.append("-Q")
                # Use kP to show stack with parameters (function arguments)
                # Note: Full local variable display would require iterating frames with dv
                command.extend(["-pv", "-p", str(target_pid), "-c", f"{sym_cmd}; ~* kP; qd"])

                result = subprocess.run(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=60,
                )
            except FileNotFoundError:
                fallback_error = debugger_error or f"{debugger_name} not available"
                return "", fallback_error
            except (subprocess.SubprocessError, OSError) as exc:
                capture_errors.append(f"PID {target_pid}: {debugger_name} failed ({exc})")
                continue

            stdout = result.stdout.strip()
            stderr = result.stderr.strip()

            output = stdout
            output_from_stderr = False
            if not output and stderr:
                output = stderr
                output_from_stderr = True

            normalized_code = self._normalize_windows_returncode(result.returncode)
            exit_ok = normalized_code in self._WINDOWS_DEBUGGER_SUCCESS_CODES

            if exit_ok:
                if output:
                    note = ""
                    if normalized_code != 0:
                        note = (
                            f"\n\n[Debugger exited with code {self._format_windows_returncode(normalized_code)};"
                            " treated as success]"
                        )
                    stack_outputs.append(f"PID {target_pid}\n{output}{note}")
                continue

            fallback_needed = False
            if not exit_ok:
                fallback_needed = self._should_use_minidump_fallback(
                    output,
                    stderr,
                    normalized_code,
                )

            if fallback_needed:
                dump_output, dump_error = self._capture_stack_via_minidump(
                    debugger_name,
                    debugger_path,
                    target_pid,
                )
                if dump_output:
                    fallback_outputs.append(
                        (f"PID {target_pid} (minidump)", dump_output, 0, "dump analysis")
                    )
                else:
                    capture_errors.append(
                        dump_error
                        or self._format_windows_debugger_failure(
                            debugger_name,
                            f"PID {target_pid}",
                            normalized_code,
                            stderr,
                        )
                    )
            else:
                if output:
                    if not output_from_stderr:
                        fallback_outputs.append((f"PID {target_pid}", output, normalized_code, stderr))
                    else:
                        capture_errors.append(
                            self._format_windows_debugger_failure(
                                debugger_name,
                                f"PID {target_pid}",
                                normalized_code,
                                stderr,
                            )
                        )
                else:
                    capture_errors.append(
                        self._format_windows_debugger_failure(
                            debugger_name,
                            f"PID {target_pid}",
                            normalized_code,
                            stderr,
                        )
                    )

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if fallback_outputs:
            annotated = []
            for label, output, normalized_code, stderr in fallback_outputs:
                detail = f"; stderr: {stderr}" if stderr else ""
                annotated.append(
                    f"{label}\n{output}\n\n[Debugger exited with code {self._format_windows_returncode(normalized_code)}; output may be incomplete{detail}]"
                )
            return "\n\n".join(annotated), "; ".join(capture_errors) if capture_errors else ""

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no stack data captured"

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

        stdout = result.stdout.strip()
        stderr = result.stderr.strip()
        output = stdout or stderr
        if not output:
            return "", "minidump analysis produced no output"
        return output, None

    def _create_minidump(self, pid: int) -> Tuple[Optional[str], Optional[str]]:
        """Generate a minidump for the given PID using comsvcs.dll."""

        system_root = os.environ.get("SystemRoot", r"C:\Windows")
        rundll32 = Path(system_root) / "System32" / "rundll32.exe"
        if not rundll32.exists():
            return None, "rundll32.exe not found for minidump creation"

        try:
            tmp_fd, tmp_path = tempfile.mkstemp(prefix=f"sintra_{pid}_", suffix=".dmp")
            os.close(tmp_fd)
        except OSError as exc:
            return None, f"failed to allocate dump file: {exc}"

        command = [
            str(rundll32),
            "comsvcs.dll, MiniDump",
            str(pid),
            tmp_path,
            "full",
        ]

        try:
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=60,
            )
        except subprocess.SubprocessError as exc:
            try:
                os.remove(tmp_path)
            except OSError:
                pass
            return None, f"minidump command failed: {exc}"

        if result.returncode != 0:
            try:
                os.remove(tmp_path)
            except OSError:
                pass
            detail = result.stderr.strip() or result.stdout.strip()
            return None, f"minidump command exited with {result.returncode}: {detail}"

        return tmp_path, None

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

    @classmethod
    def _format_windows_debugger_failure(
        cls,
        debugger_name: str,
        target: str,
        returncode: int,
        stderr: str,
    ) -> str:
        detail = f": {stderr.strip()}" if stderr else ""
        return (
            f"{target}: {debugger_name} exited with code {cls._format_windows_returncode(returncode)}{detail}"
        )

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
