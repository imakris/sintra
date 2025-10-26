#!/usr/bin/env python3
"""
Minimal test runner that builds the crash-oriented ping-pong test and
executes it once under GDB to capture stack traces from every process.
"""

import argparse
import os
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

COORDINATOR_GDB_SCRIPT = """
set pagination off
set confirm off
set debuginfod enabled off
set logging overwrite on
set logging file {log}
set logging redirect on
set logging enabled on
set follow-fork-mode parent
set detach-on-fork on
run
thread apply all bt full
set logging enabled off
quit
"""

WORKER_GDB_SCRIPT = """
set pagination off
set confirm off
set debuginfod enabled off
set logging overwrite on
set logging file {log}
set logging redirect on
set logging enabled on
thread apply all bt full
set logging enabled off
detach
quit
"""


def configure(build_dir: Path, generator: str | None) -> None:
    root = Path(__file__).resolve().parents[1]
    build_dir.mkdir(parents=True, exist_ok=True)
    cmd = ["cmake", "-S", str(root), "-B", str(build_dir)]
    if generator:
        cmd.extend(["-G", generator])
    subprocess.check_call(cmd)


def build_test(build_dir: Path) -> None:
    subprocess.check_call([
        "cmake",
        "--build",
        str(build_dir),
        "--target",
        "sintra_ping_pong_coordinator_crash_test",
    ])


def run_gdb(binary: Path, script: str, env: dict[str, str] | None = None, *, log_path: Path) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", delete=False) as handle:
        handle.write(script.format(log=log_path))
        gdb_script_path = Path(handle.name)

    try:
        cmd = ["gdb", "--batch", "-x", str(gdb_script_path), "--args", str(binary)]
        result = subprocess.run(cmd, env=env, check=False)
    finally:
        try:
            os.unlink(gdb_script_path)
        except OSError:
            pass
    return result


def attach_worker(pid: int, log_path: Path) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile("w", delete=False) as handle:
        handle.write(WORKER_GDB_SCRIPT.format(log=log_path))
        gdb_script_path = Path(handle.name)
    try:
        cmd = ["gdb", "--batch", "-x", str(gdb_script_path), "--pid", str(pid)]
        result = subprocess.run(cmd, check=False)
    finally:
        try:
            os.unlink(gdb_script_path)
        except OSError:
            pass
    return result


def ensure_worker_terminated(pid: int | None) -> None:
    if pid is None:
        return
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Build and run the coordinator crash ping-pong test")
    parser.add_argument("--build-dir", type=Path, default=Path("build-crash-test"), help="Where to configure the build")
    parser.add_argument("--generator", type=str, default=None, help="Optional CMake generator")
    args = parser.parse_args(argv)

    configure(args.build_dir, args.generator)
    build_test(args.build_dir)

    binary = args.build_dir / "tests" / "sintra_ping_pong_coordinator_crash_test"
    if not binary.exists():
        raise FileNotFoundError(f"Test binary not found: {binary}")

    fd, pid_file_raw = tempfile.mkstemp(prefix="sintra-worker-", suffix=".pid")
    os.close(fd)
    pid_file = Path(pid_file_raw)

    env = os.environ.copy()
    env["SINTRA_WORKER_PID_FILE"] = str(pid_file)

    coord_fd, coord_path = tempfile.mkstemp(prefix="sintra-coordinator-", suffix=".log")
    os.close(coord_fd)
    coordinator_log = Path(coord_path)

    run_gdb(binary, COORDINATOR_GDB_SCRIPT, env, log_path=coordinator_log)

    coordinator_output = coordinator_log.read_text()
    sys.stdout.write(coordinator_output)
    sys.stdout.flush()
    coordinator_log.unlink(missing_ok=True)

    worker_pid: int | None = None
    try:
        text = pid_file.read_text().strip()
        if not text:
            sys.stderr.write(f"Worker PID file {pid_file} was empty.\n")
        if text:
            worker_pid = int(text)
    except FileNotFoundError:
        worker_pid = None
    finally:
        try:
            pid_file.unlink()
        except FileNotFoundError:
            pass

    if worker_pid is not None:
        worker_fd, worker_path = tempfile.mkstemp(prefix="sintra-worker-", suffix=".log")
        os.close(worker_fd)
        worker_log = Path(worker_path)
        attach_worker(worker_pid, worker_log)
        sys.stdout.write("\n===== WORKER STACKS (PID {}) =====\n".format(worker_pid))
        sys.stdout.write(worker_log.read_text())
        sys.stdout.flush()
        worker_log.unlink(missing_ok=True)
    else:
        sys.stderr.write("Failed to discover worker PID in coordinator output.\n")

    ensure_worker_terminated(worker_pid)

    if "Program received signal" not in coordinator_output:
        sys.stderr.write("Expected the coordinator to crash, but GDB did not report a signal.\n")
        return 2

    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
