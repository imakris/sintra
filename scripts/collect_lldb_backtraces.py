#!/usr/bin/env python3
"""Utility to rerun failing CTest cases under LLDB in batch mode."""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build"),
        help="CMake build directory containing CTest metadata.",
    )
    parser.add_argument(
        "--config",
        default="Release",
        help="CTest configuration passed with -C/--build-config.",
    )
    parser.add_argument(
        "--failed-log",
        type=Path,
        default=None,
        help="Override path to LastTestsFailed.log (defaults under build-dir).",
    )
    parser.add_argument(
        "--lldb",
        default='xcrun lldb --batch -o run -o "thread backtrace all" --',
        help="LLDB invocation used to rerun failing tests in batch mode.",
    )
    return parser.parse_args()


def read_failed_tests(log_path: Path) -> List[str]:
    tests: List[str] = []
    if not log_path.exists():
        print(
            f"::warning::No LastTestsFailed.log found at {log_path}; unable to determine failing tests for LLDB rerun.",
            file=sys.stderr,
        )
        return tests

    with log_path.open() as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            tests.append(line)
    return tests


def collect_ctest_metadata(build_dir: Path, config: str) -> Dict[str, Tuple[Sequence[str], str | None]]:
    cmd = [
        "ctest",
        "--test-dir",
        str(build_dir),
        "-C",
        config,
        "--show-only=json-v1",
    ]
    info = subprocess.check_output(cmd, text=True)
    data = json.loads(info)

    lookup: Dict[str, Tuple[Sequence[str], str | None]] = {}
    for test in data.get("tests", []):
        name = test.get("name")
        command = [str(arg) for arg in test.get("command", []) if arg is not None]
        if not name or not command:
            continue
        workdir = None
        for prop in test.get("properties", []):
            if prop.get("name") == "WORKING_DIRECTORY":
                workdir = prop.get("value")
                break
        lookup[name] = (command, workdir)
    return lookup


def run_lldb_for_tests(
    failing_tests: Iterable[str],
    metadata: Dict[str, Tuple[Sequence[str], str | None]],
    *,
    lldb_cmd: Sequence[str],
    build_dir: Path,
) -> None:
    reruns: List[Tuple[str, Sequence[str], str | None]] = []
    for name in failing_tests:
        if name not in metadata:
            print(
                f"::warning::No command metadata found for failing test '{name}'",
                file=sys.stderr,
            )
            continue
        reruns.append((name, *metadata[name]))

    if not reruns:
        print("::warning::Unable to construct LLDB reruns for the failing tests", file=sys.stderr)
        return

    for name, command, workdir in reruns:
        print(f"::group::LLDB backtrace for {name}")
        sys.stdout.flush()
        cwd = Path(workdir) if workdir else build_dir
        try:
            subprocess.run([*lldb_cmd, *command], cwd=cwd, check=False)
        finally:
            print("::endgroup::")
            sys.stdout.flush()


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir
    failed_log = args.failed_log or build_dir / "Testing" / "Temporary" / "LastTestsFailed.log"

    failing_tests = read_failed_tests(failed_log)
    if not failing_tests:
        return 0

    try:
        metadata = collect_ctest_metadata(build_dir, args.config)
    except subprocess.CalledProcessError as exc:
        print(
            f"::warning::Failed to query CTest metadata for LLDB rerun: {exc}",
            file=sys.stderr,
        )
        return 0

    lldb_cmd = shlex.split(args.lldb)
    run_lldb_for_tests(failing_tests, metadata, lldb_cmd=lldb_cmd, build_dir=build_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
