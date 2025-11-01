from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

from .base import DebuggerStrategy

LIVE_STACK_ATTACH_TIMEOUT_ENV = "SINTRA_LIVE_STACK_ATTACH_TIMEOUT"
DEFAULT_LIVE_STACK_ATTACH_TIMEOUT = 30.0
DEFAULT_LLDB_LIVE_STACK_ATTACH_TIMEOUT = 90.0


class UnixDebuggerStrategy(DebuggerStrategy):
    """Debugger strategy for Unix-like platforms."""

    def __init__(self, verbose: bool, **kwargs) -> None:
        super().__init__(verbose, **kwargs)
        self._sudo_capability: Optional[bool] = None

    def capture_process_stacks(
        self,
        pid: int,
        process_group: Optional[int] = None,
    ) -> Tuple[str, str]:
        debugger_name, debugger_command, debugger_error = self._resolve_unix_debugger()
        if not debugger_command:
            return "", debugger_error

        try:
            import signal as signal_module
        except ImportError:  # pragma: no cover - some exotic platforms
            signal_module = None

        pgid: Optional[int] = None
        if process_group is not None:
            pgid = process_group
        else:
            try:
                pgid = os.getpgid(pid)
            except ProcessLookupError:
                return "", "process exited before stack capture"

        target_pids = {pid}
        if pgid is not None and self._collect_process_group_pids is not None:
            target_pids.update(int(p) for p in self._collect_process_group_pids(pgid))
        if self._collect_descendant_pids is not None:
            target_pids.update(int(p) for p in self._collect_descendant_pids(pid))

        if not target_pids:
            target_pids = {pid}

        def _pid_exists(target_pid: int) -> bool:
            if target_pid <= 0:
                return False
            try:
                os.kill(target_pid, 0)
            except ProcessLookupError:
                return False
            except PermissionError:
                return True
            except OSError:
                return False
            return True

        paused_pids: List[int] = []
        paused_groups: List[int] = []
        stack_outputs: List[str] = []
        capture_errors: List[str] = []

        debugger_is_macos_lldb = debugger_name == "lldb" and sys.platform == "darwin"
        should_pause = signal_module is not None and not debugger_is_macos_lldb

        if should_pause and signal_module is not None:
            if pgid is not None and pgid > 0:
                try:
                    if os.getpgrp() != pgid:
                        os.killpg(pgid, signal_module.SIGSTOP)
                        paused_groups.append(pgid)
                except (ProcessLookupError, PermissionError, OSError):
                    pass

            for target_pid in sorted(target_pids):
                if target_pid == os.getpid():
                    continue
                if not _pid_exists(target_pid):
                    continue
                try:
                    os.kill(target_pid, signal_module.SIGSTOP)
                    paused_pids.append(target_pid)
                except (ProcessLookupError, PermissionError):
                    continue

        if debugger_is_macos_lldb:
            ordered_target_pids = sorted(target_pids, reverse=True)
        else:
            ordered_target_pids = sorted(target_pids)

        env_timeout = os.environ.get(LIVE_STACK_ATTACH_TIMEOUT_ENV)
        attach_timeout = 0.0
        if env_timeout:
            try:
                attach_timeout = float(env_timeout)
            except ValueError:
                attach_timeout = 0.0

        if attach_timeout <= 0.0:
            if debugger_is_macos_lldb:
                attach_timeout = DEFAULT_LLDB_LIVE_STACK_ATTACH_TIMEOUT
            else:
                attach_timeout = DEFAULT_LIVE_STACK_ATTACH_TIMEOUT

        per_pid_timeout = min(30.0, attach_timeout)
        total_budget = attach_timeout
        capture_deadline = time.monotonic() + total_budget
        attach_timeout_seconds = attach_timeout

        attach_timeout_hint_needed = False

        for target_pid in ordered_target_pids:
            if target_pid == os.getpid():
                continue
            if not _pid_exists(target_pid):
                capture_errors.append(
                    f"PID {target_pid}: process exited before stack capture"
                )
                continue

            remaining = capture_deadline - time.monotonic()
            if remaining <= 0:
                capture_errors.append(
                    f"PID {target_pid}: skipped stack capture (overall debugger timeout exceeded)"
                )
                attach_timeout_hint_needed = True
                break
            debugger_timeout = min(
                remaining,
                max(5.0, min(per_pid_timeout, remaining)),
            )

            debugger_output = ""
            debugger_error = ""
            debugger_success = False

            max_attempts = 3 if debugger_is_macos_lldb else 1
            base_command = self._build_unix_live_debugger_command(
                debugger_name,
                debugger_command,
                target_pid,
            )
            attempt = 0
            use_sudo = False
            while attempt < max_attempts:
                command = (
                    self._wrap_command_with_sudo(base_command)
                    if use_sudo
                    else base_command
                )
                try:
                    result = subprocess.run(
                        command,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        text=True,
                        timeout=debugger_timeout,
                    )
                except subprocess.TimeoutExpired:
                    debugger_error = (
                        f"{debugger_name} timed out after {debugger_timeout:.1f}s while attaching"
                    )
                    attach_timeout_hint_needed = True
                    break
                except subprocess.SubprocessError as exc:
                    debugger_error = f"{debugger_name} failed ({exc})"
                    break

                debugger_output = result.stdout.strip()
                if not debugger_output and result.stderr:
                    debugger_output = result.stderr.strip()

                if result.returncode == 0:
                    debugger_success = True
                    break

                debugger_error = (
                    f"{debugger_name} exited with code {result.returncode}: {result.stderr.strip()}"
                )

                if (
                    not use_sudo
                    and self._should_retry_with_sudo(result.stderr, result.stdout)
                ):
                    use_sudo = True
                    continue

                if not debugger_is_macos_lldb:
                    break

                if not self._process_exists(target_pid):
                    break

                lowered_error = result.stderr.strip().lower()
                if "no such process" not in lowered_error and "does not exist" not in lowered_error:
                    break

                time.sleep(0.5)

                attempt += 1

            if debugger_success:
                if debugger_output:
                    stack_outputs.append(f"PID {target_pid}\n{debugger_output}")
                continue

            fallback_output = ""
            fallback_error = debugger_error

            if debugger_is_macos_lldb and self._process_exists(target_pid):
                fallback_output, fallback_error = self._capture_macos_sample_stack(
                    target_pid,
                    debugger_timeout,
                )

            if fallback_output:
                stack_outputs.append(f"PID {target_pid}\n{fallback_output}")
                continue

            if fallback_error:
                capture_errors.append(f"PID {target_pid}: {fallback_error}")

        if should_pause and signal_module is not None:
            for target_pid in paused_pids:
                try:
                    os.kill(target_pid, signal_module.SIGCONT)
                except (ProcessLookupError, PermissionError):
                    continue

            for group_id in paused_groups:
                try:
                    if os.getpgrp() != group_id:
                        os.killpg(group_id, signal_module.SIGCONT)
                except (ProcessLookupError, PermissionError, OSError):
                    continue

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if attach_timeout_hint_needed:
            capture_errors.append(
                (
                    "Live stack capture timed out after "
                    f"{attach_timeout_seconds:.1f}s; increase "
                    f"{LIVE_STACK_ATTACH_TIMEOUT_ENV} to allow more debugger time"
                )
            )

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no stack data captured"

    def capture_core_dump_stack(
        self,
        invocation: "TestInvocation",
        start_time: float,
        pid: int,
    ) -> Tuple[str, str]:
        debugger_name, debugger_command, debugger_error = self._resolve_unix_debugger()
        if not debugger_command:
            return "", debugger_error

        candidate_dirs = [invocation.path.parent, Path.cwd()]

        if sys.platform == "darwin":
            darwin_core_dirs = [Path("/cores"), Path.home() / "Library" / "Logs" / "DiagnosticReports"]
            for directory in darwin_core_dirs:
                if directory not in candidate_dirs:
                    candidate_dirs.append(directory)

        candidate_cores: List[Tuple[float, Path]] = []

        for directory in candidate_dirs:
            try:
                if not directory.exists():
                    continue
                entries = list(directory.iterdir())
            except OSError:
                continue

            for entry in entries:
                if not entry.is_file():
                    continue

                name_lower = entry.name.lower()
                exe_name = invocation.path.name.lower()
                if exe_name not in name_lower and invocation.path.stem.lower() not in name_lower:
                    continue

                try:
                    stat_info = entry.stat()
                except OSError:
                    continue

                if stat_info.st_mtime + 0.001 < start_time:
                    continue

                candidate_cores.append((stat_info.st_mtime, entry))

        if not candidate_cores:
            return "", "no recent core dump found"

        candidate_cores.sort(key=lambda item: item[0], reverse=True)

        stack_outputs: List[str] = []
        capture_errors: List[str] = []

        for _, core_path in candidate_cores:
            command = self._build_unix_core_debugger_command(
                debugger_name,
                debugger_command,
                invocation,
                core_path,
            )

            try:
                result = subprocess.run(
                    command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=180,
                )
            except subprocess.SubprocessError as exc:
                capture_errors.append(f"{core_path}: debugger failed ({exc})")
                continue

            output = result.stdout.strip()
            if not output and result.stderr:
                output = result.stderr.strip()

            if result.returncode == 0 and output:
                stack_outputs.append(f"{core_path}\n{output}")
                break

            if output:
                capture_errors.append(
                    f"{core_path}: debugger exited with code {result.returncode}: {output}"
                )
            else:
                capture_errors.append(
                    f"{core_path}: debugger exited with code {result.returncode} without output"
                )

        if stack_outputs:
            return "\n\n".join(stack_outputs), ""

        if capture_errors:
            return "", "; ".join(capture_errors)

        return "", "no stack data captured"

    def _resolve_unix_debugger(self) -> Tuple[Optional[str], Optional[List[str]], str]:
        if sys.platform == "win32":
            return None, None, "debugger not available on this platform"

        gdb_path = shutil.which("gdb")
        if gdb_path:
            return "gdb", [gdb_path], ""

        lldb_path = shutil.which("lldb")
        if lldb_path:
            return "lldb", [lldb_path], ""

        xcrun_path = shutil.which("xcrun")
        if xcrun_path:
            return "lldb", [xcrun_path, "lldb"], ""

        return None, None, "gdb or lldb not available (install gdb or the Xcode Command Line Tools)"

    def _build_unix_live_debugger_command(
        self,
        debugger_name: str,
        debugger_command: List[str],
        pid: int,
    ) -> List[str]:
        if debugger_name == "gdb":
            return [
                *debugger_command,
                "--batch",
                "--quiet",
                "--nx",
                "-ex",
                "set confirm off",
                "-ex",
                "set pagination off",
                "-ex",
                "thread apply all bt full",
                "-ex",
                "detach",
                "-ex",
                "quit",
                str(pid),
            ]

        return [
            *debugger_command,
            "--batch",
            "--no-lldbinit",
            "-p",
            str(pid),
            "-o",
            "thread backtrace all -c 256 -f",
            "-o",
            "detach",
            "-o",
            "quit",
        ]

    def _build_unix_core_debugger_command(
        self,
        debugger_name: str,
        debugger_command: List[str],
        invocation: "TestInvocation",
        core_path: Path,
    ) -> List[str]:
        if debugger_name == "gdb":
            return [
                *debugger_command,
                "--batch",
                "--quiet",
                "--nx",
                "-ex",
                "set confirm off",
                "-ex",
                "set pagination off",
                "-ex",
                "thread apply all bt full",
                "-ex",
                "quit",
                str(invocation.path),
                str(core_path),
            ]

        quoted_executable = shlex.quote(str(invocation.path))
        quoted_core = shlex.quote(str(core_path))

        return [
            *debugger_command,
            "--batch",
            "--no-lldbinit",
            "-o",
            f"target create --core {quoted_core} {quoted_executable}",
            "-o",
            "thread backtrace all -c 256 -f",
            "-o",
            "quit",
        ]

    def _capture_macos_sample_stack(
        self,
        pid: int,
        timeout: float,
    ) -> Tuple[str, str]:
        sample_path = shutil.which("sample")
        if not sample_path:
            return "", "sample utility not available"

        duration = max(1, min(10, int(timeout)))
        command = [
            sample_path,
            str(pid),
            str(duration),
            "1",
            "-mayDie",
        ]

        sudo_supported = self._supports_passwordless_sudo()
        for use_sudo in (False, True) if sudo_supported else (False,):
            actual_command = (
                self._wrap_command_with_sudo(command) if use_sudo else command
            )

            try:
                result = subprocess.run(
                    actual_command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                    timeout=duration + 5,
                )
            except subprocess.SubprocessError as exc:
                error = f"sample failed ({exc})"
                if not use_sudo and sudo_supported:
                    continue
                return "", error

            if result.returncode == 0:
                output = result.stdout.strip()
                if not output:
                    return "", "sample produced no output"
                return output, ""

            detail = result.stderr.strip() or result.stdout.strip()
            error_message = (
                f"sample exited with code {result.returncode}: {detail}"
                if detail
                else f"sample exited with code {result.returncode}"
            )

            if use_sudo or not self._should_retry_with_sudo(result.stderr, result.stdout):
                return "", error_message

        return "", "sample exited without producing output"

    def _supports_passwordless_sudo(self) -> bool:
        if self._sudo_capability is not None:
            return self._sudo_capability

        if sys.platform != "darwin":
            self._sudo_capability = False
            return False

        if hasattr(os, "geteuid") and os.geteuid() == 0:
            self._sudo_capability = False
            return False

        sudo_path = shutil.which("sudo")
        if not sudo_path:
            self._sudo_capability = False
            return False

        try:
            result = subprocess.run(
                [sudo_path, "-n", "true"],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=5,
            )
        except subprocess.SubprocessError:
            self._sudo_capability = False
            return False

        self._sudo_capability = result.returncode == 0
        return self._sudo_capability

    def _wrap_command_with_sudo(self, command: List[str]) -> List[str]:
        if not self._supports_passwordless_sudo():
            return command
        return ["sudo", "-n", *command]

    @staticmethod
    def _should_retry_with_sudo(stderr: str, stdout: str) -> bool:
        combined = f"{stderr}\n{stdout}".lower()
        retry_markers = (
            "operation not permitted",
            "permission denied",
            "requires root",
            "must be run as root",
            "could not attach",
        )
        return any(marker in combined for marker in retry_markers)

    @staticmethod
    def _process_exists(pid: int) -> bool:
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
