from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from .base import DebuggerStrategy

WINDOWS_DEBUGGER_CACHE_ENV = "SINTRA_WINDOWS_DEBUGGER_CACHE"
WINSDK_INSTALLER_URL_ENV = "SINTRA_WINSDK_INSTALLER_URL"
WINSDK_FEATURE_ENV = "SINTRA_WINSDK_FEATURE"
WINSDK_DEBUGGER_MSI_ENV = "SINTRA_WINSDK_DEBUGGER_MSI"
WINSDK_INSTALLER_URL = os.environ.get(
    WINSDK_INSTALLER_URL_ENV,
    "https://download.microsoft.com/download/7/9/6/7962e9ce-cd69-4574-978c-1202654bd729/windowssdk/winsdksetup.exe",
)
WINSDK_FEATURE_ID = os.environ.get(WINSDK_FEATURE_ENV, "OptionId.WindowsDesktopDebuggers")
WINSDK_DEBUGGER_MSI_NAME = os.environ.get(
    WINSDK_DEBUGGER_MSI_ENV,
    "X64 Debuggers And Tools-x64_en-us.msi",
)


class WindowsDebuggerStrategy(DebuggerStrategy):
    """Debugger strategy for Windows hosts."""

    _WINDOWS_DEBUGGER_SUCCESS_CODES = {0x00000000, 0xD000010A}

    def __init__(self, verbose: bool, **kwargs) -> None:
        super().__init__(verbose, **kwargs)
        self._debugger_cache: Dict[str, Tuple[Optional[str], str]] = {}
        self._downloaded_windows_debugger_root: Optional[Path] = None
        self._windows_crash_dump_dir: Optional[Path] = None

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
        return self._ensure_windows_local_dumps()

    def configure_jit_debugging(self) -> Optional[str]:
        return self._configure_windows_jit_debugging()

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

    # Downloader helpers ------------------------------------------------
    def _locate_windows_debugger(self, executable: str) -> Tuple[Optional[str], str]:
        if sys.platform != "win32":
            return None, f"{executable} not available on this platform"

        cache_key = executable.lower()
        if cache_key in self._debugger_cache:
            return self._debugger_cache[cache_key]

        debugger_root, prepare_error = self._ensure_downloaded_windows_debugger_root()
        if not debugger_root:
            error = prepare_error or f"failed to prepare debugger payload for {executable}"
            result = (None, error)
            self._debugger_cache[cache_key] = result
            return result

        candidates = [executable]
        if not executable.lower().endswith(".exe"):
            candidates.append(f"{executable}.exe")

        located = self._find_downloaded_debugger_executable(debugger_root, candidates)
        if located:
            result = (str(located), "")
            self._debugger_cache[cache_key] = result
            return result

        error = f"{executable} not found in downloaded debugger cache ({debugger_root})"
        result = (None, error)
        self._debugger_cache[cache_key] = result
        return result

    def _ensure_downloaded_windows_debugger_root(self) -> Tuple[Optional[Path], str]:
        if self._downloaded_windows_debugger_root:
            return self._downloaded_windows_debugger_root, ""

        cache_dir = self._get_windows_debugger_cache_dir()
        try:
            cache_dir.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return None, f"failed to create debugger cache directory {cache_dir}: {exc}"

        install_root = cache_dir / "winsdk_debuggers"
        debugger_root = install_root / "Windows Kits" / "10" / "Debuggers"
        sentinel = debugger_root / "x64" / "cdb.exe"
        try:
            if sentinel.exists():
                self._downloaded_windows_debugger_root = debugger_root
                return debugger_root, ""
        except OSError:
            pass

        layout_dir = cache_dir / "winsdk_layout"
        msi_path = layout_dir / "Installers" / WINSDK_DEBUGGER_MSI_NAME

        if not msi_path.exists():
            layout_error = self._ensure_winsdk_layout(layout_dir)
            if layout_error:
                return None, layout_error
            if not msi_path.exists():
                return None, (
                    f"expected debugger MSI {WINSDK_DEBUGGER_MSI_NAME} missing from layout ({layout_dir})"
                )

        extract_error = self._extract_debugger_msi(msi_path, install_root)
        if extract_error:
            return None, extract_error

        try:
            if not sentinel.exists():
                return None, f"debugger executable not found at {sentinel}"
        except OSError as exc:
            return None, f"failed to verify debugger installation at {sentinel}: {exc}"

        self._downloaded_windows_debugger_root = debugger_root
        return debugger_root, ""

    def _ensure_winsdk_layout(self, layout_dir: Path) -> Optional[str]:
        installer_path, installer_error = self._ensure_winsdk_installer()
        if installer_error:
            return installer_error
        assert installer_path is not None

        if layout_dir.exists():
            shutil.rmtree(layout_dir, ignore_errors=True)

        try:
            layout_dir.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return f"failed to create layout directory {layout_dir}: {exc}"

        log_path = layout_dir.parent / "winsdksetup-layout.log"
        command = [
            str(installer_path),
            "/quiet",
            "/norestart",
            "/layout",
            str(layout_dir),
            "/features",
            WINSDK_FEATURE_ID,
            "/log",
            str(log_path),
        ]

        try:
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=3600,
            )
        except (subprocess.SubprocessError, OSError) as exc:
            return f"winsdksetup.exe failed to create layout: {exc}"

        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            return (
                f"winsdksetup.exe layout failed with exit code {result.returncode}: "
                f"{detail or f'see {log_path} for details'}"
            )

        return None

    def _ensure_winsdk_installer(self) -> Tuple[Optional[Path], Optional[str]]:
        cache_dir = self._get_windows_debugger_cache_dir()
        installer_path = cache_dir / "winsdksetup.exe"
        if installer_path.exists():
            return installer_path, None

        download_error = self._download_file(WINSDK_INSTALLER_URL, installer_path)
        if download_error:
            return None, f"failed to download WinSDK installer: {download_error}"

        return installer_path, None

    def _extract_debugger_msi(self, msi_path: Path, destination: Path) -> Optional[str]:
        if destination.exists():
            shutil.rmtree(destination, ignore_errors=True)

        extract_error = self._extract_msi_package(msi_path, destination)
        if extract_error:
            shutil.rmtree(destination, ignore_errors=True)
            return extract_error

        return None

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

    def _download_file(self, url: str, destination: Path) -> Optional[str]:
        try:
            destination.parent.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return f"failed to create directory for {destination}: {exc}"

        temp_file = tempfile.NamedTemporaryFile(
            delete=False, dir=str(destination.parent), suffix=".tmp"
        )
        temp_path = Path(temp_file.name)
        try:
            with urllib.request.urlopen(url) as response:
                chunk_size = 1024 * 1024
                while True:
                    chunk = response.read(chunk_size)
                    if not chunk:
                        break
                    temp_file.write(chunk)
            temp_file.close()
            os.replace(temp_path, destination)
            return None
        except urllib.error.URLError as exc:
            temp_file.close()
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass
            return f"failed to download {url}: {exc}"
        except OSError as exc:
            temp_file.close()
            try:
                temp_path.unlink()
            except FileNotFoundError:
                pass
            return f"failed to write {destination}: {exc}"

    def _extract_msi_package(self, package: Path, destination: Path) -> Optional[str]:
        msiexec = shutil.which("msiexec")
        if not msiexec:
            windir = os.environ.get("WINDIR")
            if windir:
                candidate = Path(windir) / "System32" / "msiexec.exe"
                if candidate.exists():
                    msiexec = str(candidate)

        if not msiexec:
            return "'msiexec' not available to extract debugger package"

        try:
            destination.mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            return f"failed to create extraction directory {destination}: {exc}"

        try:
            result = subprocess.run(
                [
                    msiexec,
                    "/a",
                    str(package),
                    "/qn",
                    f"TARGETDIR={destination}",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=900,
            )
        except (subprocess.SubprocessError, OSError) as exc:
            return f"msiexec failed to extract debugger package: {exc}"

        if result.returncode != 0:
            detail = result.stderr.strip() or result.stdout.strip()
            return f"msiexec exited with {result.returncode}: {detail}"

        return None

    def _find_downloaded_debugger_executable(
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

    # Windows configuration helpers -------------------------------------
    def _prepare_windows_debuggers(self) -> None:
        # Retained for backwards compatibility; prepare() calls this indirectly.
        self.prepare()

    def _ensure_windows_local_dumps(self) -> Optional[str]:
        if sys.platform != "win32":
            return None

        if self._windows_crash_dump_dir is not None:
            return None

        try:
            import winreg  # type: ignore
        except ImportError as exc:  # pragma: no cover - Windows specific
            return f"winreg unavailable: {exc}"

        reg_subkey = r"Software\Microsoft\Windows\Windows Error Reporting\LocalDumps"
        desired_folder_value = r"%LOCALAPPDATA%\CrashDumps"

        access_flags = winreg.KEY_READ | winreg.KEY_SET_VALUE
        try:
            key = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, reg_subkey, 0, access_flags)
        except OSError as exc:
            return f"failed to open registry key HKCU\\{reg_subkey}: {exc}"

        with key:
            local_app_data = os.environ.get("LOCALAPPDATA")
            if local_app_data:
                default_dump_dir = Path(local_app_data) / "CrashDumps"
                folder_value_to_set = desired_folder_value
                folder_value_type = winreg.REG_EXPAND_SZ
            else:
                default_dump_dir = Path.home() / "AppData" / "Local" / "CrashDumps"
                folder_value_to_set = str(default_dump_dir)
                folder_value_type = winreg.REG_SZ

            try:
                existing_folder_value, _ = winreg.QueryValueEx(key, "DumpFolder")
            except FileNotFoundError:
                existing_folder_value = None

            if existing_folder_value:
                expanded = os.path.expandvars(existing_folder_value)
                dump_dir = Path(expanded).expanduser()
                if not dump_dir.is_absolute():
                    dump_dir = default_dump_dir
            else:
                dump_dir = default_dump_dir
                try:
                    winreg.SetValueEx(key, "DumpFolder", 0, folder_value_type, folder_value_to_set)
                except OSError as exc:
                    return f"failed to configure dump folder HKCU\\{reg_subkey}: {exc}"

            try:
                existing_type = winreg.QueryValueEx(key, "DumpType")[1]
            except FileNotFoundError:
                existing_type = None

            if existing_type != winreg.REG_DWORD:
                try:
                    winreg.SetValueEx(key, "DumpType", 0, winreg.REG_DWORD, 2)
                except OSError as exc:
                    return f"failed to set DumpType for HKCU\\{reg_subkey}: {exc}"

            try:
                dump_dir.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                return f"failed to create dump directory {dump_dir}: {exc}"

            self._windows_crash_dump_dir = dump_dir

        return None

    def _configure_windows_jit_debugging(self) -> Optional[str]:
        """Disable intrusive JIT prompts so crash dumps complete automatically."""
        if sys.platform != "win32":
            return None

        try:
            import winreg  # type: ignore
        except ImportError as exc:  # pragma: no cover - Windows specific
            return f"winreg unavailable: {exc}"

        errors: List[str] = []

        def set_dword(root: int, subkey: str, name: str, value: int, access: int) -> None:
            try:
                handle = winreg.CreateKeyEx(root, subkey, 0, access)
            except OSError as exc:
                errors.append(f"{subkey}: failed to open key: {exc}")
                return
            with handle:
                try:
                    winreg.SetValueEx(handle, name, 0, winreg.REG_DWORD, value)
                except OSError as exc:
                    errors.append(f"{subkey}: failed to set {name}: {exc}")

        # Set Auto=1 for AeDebug to automatically perform default action (create crash dump)
        # instead of showing a prompt that would block in headless CI environments
        set_dword(
            winreg.HKEY_LOCAL_MACHINE,
            r"Software\Microsoft\Windows NT\CurrentVersion\AeDebug",
            "Auto",
            1,
            winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY,
        )

        # Disable Windows Error Reporting UI dialogs
        set_dword(
            winreg.HKEY_LOCAL_MACHINE,
            r"Software\Microsoft\Windows\Windows Error Reporting",
            "DontShowUI",
            1,
            winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY,
        )

        # Configure .NET JIT debugging to not launch debugger on unhandled exceptions
        jit_subkeys = [
            (winreg.HKEY_LOCAL_MACHINE, r"Software\Microsoft\.NETFramework"),
            (winreg.HKEY_LOCAL_MACHINE, r"Software\Wow6432Node\Microsoft\.NETFramework"),
            (winreg.HKEY_CURRENT_USER, r"Software\Microsoft\.NETFramework"),
        ]

        for root, subkey in jit_subkeys:
            access = winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY
            if root == winreg.HKEY_CURRENT_USER:
                access = winreg.KEY_SET_VALUE
            elif "Wow6432Node" in subkey:
                access = winreg.KEY_SET_VALUE | winreg.KEY_WOW64_32KEY
            set_dword(root, subkey, "DbgJITDebugLaunchSetting", 2, access)

        if errors:
            return "; ".join(errors)

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
    def _capture_windows_crash_dump(
        self,
        invocation: "TestInvocation",
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        debugger_name, debugger_path, debugger_error = self._resolve_windows_debugger()
        if not debugger_path:
            return "", debugger_error

        candidate_dirs = [invocation.path.parent, Path.cwd()]

        if self._windows_crash_dump_dir:
            candidate_dirs.insert(0, self._windows_crash_dump_dir)

        local_app_data = os.environ.get("LOCALAPPDATA")
        if local_app_data:
            candidate_dirs.append(Path(local_app_data) / "CrashDumps")
        else:
            candidate_dirs.append(Path.home() / "AppData" / "Local" / "CrashDumps")

        exe_name_lower = invocation.path.name.lower()
        exe_stem_lower = invocation.path.stem.lower()
        pid_str = str(pid)

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

                try:
                    stat_info = entry.stat()
                except OSError:
                    continue

                if stat_info.st_mtime + 0.001 < start_time:
                    continue

                candidate_dumps.append((stat_info.st_mtime, entry))

        if not candidate_dumps:
            return "", "no recent crash dump found"

        prioritized = [item for item in candidate_dumps if pid_str in item[1].name]
        if prioritized:
            candidate_dumps = prioritized

        candidate_dumps.sort(key=lambda item: item[0], reverse=True)

        stack_outputs: List[str] = []
        capture_errors: List[str] = []
        fallback_outputs: List[Tuple[str, str, int, str]] = []

        for _, dump_path in candidate_dumps:
            try:
                command = [debugger_path]
                if debugger_name == "windbg":
                    command.append("-Q")
                # Use kP to show stack with parameters (function arguments)
                command.extend(["-z", str(dump_path), "-c", ".symfix; .reload; ~* kP; qd"])

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
                command.extend(["-pv", "-p", str(target_pid), "-c", ".symfix; .reload; ~* kP; qd"])

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

            command = [debugger_path]
            if debugger_name == "windbg":
                command.append("-Q")
            command.extend(["-z", str(dump_path), "-c", ".symfix; .reload; ~* kP; qd"])

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

    # Legacy compatibility ----------------------------------------------
    def _capture_process_stacks(self, pid: int, process_group: Optional[int] = None) -> Tuple[str, str]:
        return self.capture_process_stacks(pid, process_group)
