#!/usr/bin/env python3
"""Minimal test runner for the crashing ping-pong scenario."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = REPO_ROOT / "build"
TEST_TARGET = "sintra_ping_pong_crash_test"


def run_command(command, *, cwd=None):
    print("+", " ".join(str(part) for part in command))
    result = subprocess.run(command, cwd=cwd)
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, command)
    return result


def configure(build_dir: Path):
    run_command([
        "cmake",
        "-S",
        str(REPO_ROOT),
        "-B",
        str(build_dir),
        "-DSINTRA_BUILD_EXAMPLES=OFF",
        "-DSINTRA_BUILD_TESTS=ON",
    ])


def build(build_dir: Path, config: Optional[str]):
    command = ["cmake", "--build", str(build_dir), "--target", TEST_TARGET]
    if config:
        command.extend(["--config", config])
    run_command(command)


def resolve_executable(build_dir: Path, config: Optional[str]) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    candidates = []
    if config:
        candidates.append(build_dir / "tests" / config / f"{TEST_TARGET}{suffix}")
    candidates.append(build_dir / "tests" / f"{TEST_TARGET}{suffix}")
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "Could not locate built executable. Looked in:\n" +
        "\n".join(str(path) for path in candidates)
    )


def run_under_gdb(executable: Path) -> int:
    gdb = shutil.which("gdb")
    if not gdb:
        raise RuntimeError("gdb is required to capture stack traces but was not found in PATH")

    inferior_dump = (
        "python import gdb; "
        "[gdb.write('\\n===== Process %d (PID %s) =====\\n' % (inf.num, inf.pid)) or "
        "gdb.execute('inferior %d' % inf.num, to_string=False) or "
        "gdb.execute('thread apply all bt', to_string=False) for inf in gdb.inferiors()]"
    )

    command = [
        gdb,
        "--quiet",
        "--batch",
        "-ex",
        "set pagination off",
        "-ex",
        "set confirm off",
        "-ex",
        "set print thread-events off",
        "-ex",
        "set detach-on-fork off",
        "-ex",
        "set follow-fork-mode parent",
        "-ex",
        "set schedule-multiple on",
        "-ex",
        "run",
        "-ex",
        inferior_dump,
        "-ex",
        "quit 1",
        str(executable),
    ]

    print("+", " ".join(command))
    completed = subprocess.run(command)
    return completed.returncode


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=DEFAULT_BUILD_DIR,
        help="Directory where CMake will generate build files",
    )
    parser.add_argument(
        "--config",
        help="Configuration name for multi-config generators (e.g. Debug)",
    )
    parser.add_argument(
        "--skip-configure",
        action="store_true",
        help="Assume the build directory is already configured",
    )
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    build_dir = args.build_dir.resolve()
    build_dir.mkdir(parents=True, exist_ok=True)

    if not args.skip_configure:
        configure(build_dir)

    build(build_dir, args.config)
    executable = resolve_executable(build_dir, args.config)
    return run_under_gdb(executable)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
