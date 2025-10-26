#!/usr/bin/env python3
"""Minimal test harness for the crashy ping-pong scenario.

This runner executes exactly one test binary. The test intentionally crashes,
so the runner forwards its standard streams verbatim and propagates the exit
code without retries, filters, or environment discovery. When the executable is
missing an informative error is emitted.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

TEST_NAME = "sintra_ping_pong_crash_test"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the single Sintra crash test")
    parser.add_argument(
        "--build-dir",
        default=Path(__file__).resolve().parent.parent / "build",
        type=Path,
        help="CMake build directory containing the compiled test binary",
    )
    parser.add_argument(
        "--config",
        default="Debug",
        help=(
            "Configuration subdirectory to look for the executable when using "
            "multi-config generators (default: Debug)"
        ),
    )
    return parser.parse_args()


def locate_executable(build_dir: Path, config: str) -> Path:
    candidates = [
        build_dir / "tests" / config / TEST_NAME,
        build_dir / "tests" / f"{TEST_NAME}_{config.lower()}",
        build_dir / "tests" / TEST_NAME,
    ]
    for candidate in candidates:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return candidate
    return Path()


def run_test(executable: Path) -> int:
    print(f"Running {TEST_NAME} -> {executable}")
    process = subprocess.Popen(
        [str(executable)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, stderr = process.communicate()

    if stdout:
        print("--- stdout ---")
        print(stdout, end="")
    if stderr:
        print("--- stderr ---", file=sys.stderr)
        print(stderr, end="", file=sys.stderr)

    if process.returncode == 0:
        print("Test unexpectedly succeeded (it should crash).", file=sys.stderr)
    else:
        print(
            f"Test exited with code {process.returncode}. This is expected for the crash scenario.",
            file=sys.stderr,
        )
    return process.returncode


def main() -> int:
    args = parse_args()
    executable = locate_executable(args.build_dir, args.config)
    if not executable:
        print(
            f"Unable to find {TEST_NAME} in {args.build_dir}. Did you build the tests?",
            file=sys.stderr,
        )
        return 1
    return run_test(executable)


if __name__ == "__main__":
    sys.exit(main())
