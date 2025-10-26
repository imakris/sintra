#!/usr/bin/env python3
"""Minimal driver that rebuilds and executes the crashy ping-pong test once.

The intent of this script is to provide a tiny, deterministic harness that
triggers the intentional coordinator crash and captures stack traces for both
processes participating in the ping-pong exchange.
"""

from __future__ import annotations

import argparse
import os
import resource
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable, List


def ensure_tool(name: str) -> None:
    if shutil.which(name) is None:
        raise RuntimeError(f"Required tool '{name}' is not available in PATH")


def run_command(
    cmd: Iterable[str], *, cwd: Path | None = None, env: dict[str, str] | None = None,
    timeout: float | None = None,
) -> subprocess.CompletedProcess:
    print(f"\n$ {' '.join(shlex.quote(part) for part in cmd)}")
    process = subprocess.Popen(
        list(cmd), cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    def as_text(chunk: str | bytes | None) -> str:
        if chunk is None:
            return ""
        if isinstance(chunk, bytes):
            return chunk.decode(errors="replace")
        return chunk

    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        process.kill()
        try:
            remainder_stdout, remainder_stderr = process.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            remainder_stdout, remainder_stderr = "", ""

        stdout = as_text(exc.stdout) + as_text(remainder_stdout)
        stderr = as_text(exc.stderr) + as_text(remainder_stderr)
        print(stdout, end="")
        print(stderr, end="", file=sys.stderr)
        return subprocess.CompletedProcess(cmd, process.returncode if process.returncode is not None else -1, stdout, stderr)

    stdout = as_text(stdout)
    stderr = as_text(stderr)

    if stdout:
        print(stdout, end="")
    if stderr:
        print(stderr, end="", file=sys.stderr)
    if process.returncode != 0:
        print(f"command exited with status {process.returncode}", file=sys.stderr)

    return subprocess.CompletedProcess(cmd, process.returncode if process.returncode is not None else -1, stdout, stderr)


def configure_build(build_dir: Path) -> None:
    build_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        "cmake",
        "-S",
        str(Path(__file__).resolve().parents[1]),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DSINTRA_BUILD_EXAMPLES=OFF",
        "-DSINTRA_BUILD_TESTS=ON",
    ]
    result = run_command(cmd)
    if result.returncode != 0:
        raise RuntimeError("cmake configuration failed")


def build_test(build_dir: Path) -> Path:
    cmd = ["cmake", "--build", str(build_dir), "--target", "sintra_ping_pong_crash_test"]
    result = run_command(cmd)
    if result.returncode != 0:
        raise RuntimeError("build failed")

    # Detect the final executable location.
    candidate = build_dir / "tests" / "sintra_ping_pong_crash_test"
    if candidate.exists():
        return candidate
    raise RuntimeError(f"Unable to locate built test binary at {candidate}")


def read_pid_file(shared_dir: Path, role: str) -> int:
    path = shared_dir / f"{role}.pid"
    data = path.read_text().strip()
    return int(data)


def gdb_run(binary: Path, shared_dir: Path) -> subprocess.CompletedProcess:
    ensure_tool("gdb")
    env = os.environ.copy()
    env["SINTRA_TEST_SHARED_DIR"] = str(shared_dir)
    cmd: List[str] = [
        "gdb",
        "-batch",
        "-q",
        "-ex",
        "set pagination off",
        "-ex",
        "set confirm off",
        "-ex",
        "set follow-fork-mode parent",
        "-ex",
        f"set env SINTRA_TEST_SHARED_DIR {shared_dir}",
        "-ex",
        "run",
        "-ex",
        "thread apply all bt",
        "-ex",
        "quit",
        "--args",
        str(binary),
    ]
    # Enable core dumps for the child process so gdb can inspect it fully.
    resource.setrlimit(resource.RLIMIT_CORE, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))
    return run_command(cmd, env=env, timeout=15.0)


def gdb_attach(pid: int) -> subprocess.CompletedProcess:
    ensure_tool("gdb")
    cmd = [
        "gdb",
        "-batch",
        "-q",
        "-ex",
        "set pagination off",
        "-ex",
        "thread apply all bt",
        "-ex",
        "detach",
        "-ex",
        "quit",
        "-p",
        str(pid),
    ]
    return run_command(cmd, timeout=10.0)


def terminate_process(pid: int) -> None:
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the crashy ping-pong test exactly once")
    parser.add_argument("--build-dir", type=Path, default=Path("build"), help="Build directory")
    args = parser.parse_args()

    build_dir: Path = args.build_dir.resolve()
    configure_build(build_dir)
    binary = build_test(build_dir)

    with tempfile.TemporaryDirectory(prefix="sintra_ping_pong_") as tmp:
        shared_dir = Path(tmp)
        print(f"Using shared directory: {shared_dir}")

        gdb_result = gdb_run(binary, shared_dir)
        if gdb_result.returncode == 0 and "Program exited normally" in gdb_result.stdout:
            print("Expected the coordinator to crash, but the program exited normally", file=sys.stderr)
            return 1

        # Collect worker stack traces if the pid file exists.
        try:
            worker_pid = read_pid_file(shared_dir, "worker")
        except FileNotFoundError:
            worker_pid = None
        except ValueError:
            worker_pid = None

        if worker_pid is not None:
            print(f"\nAttaching to worker process {worker_pid} for stack traces...")
            attach_result = gdb_attach(worker_pid)
            terminate_process(worker_pid)
            if attach_result.returncode != 0:
                print("Failed to capture worker stack traces", file=sys.stderr)
        else:
            print("Worker PID file was not created; unable to collect worker stack traces", file=sys.stderr)

    return 1


if __name__ == "__main__":
    sys.exit(main())
