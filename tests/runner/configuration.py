from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, Optional


TEST_TIMEOUT_OVERRIDES = {
    "recovery_test_debug": 120.0,
    "recovery_test_release": 120.0,
}


def parse_active_tests(tests_dir: Path) -> Dict[str, int]:
    """Parse active_tests.txt and return dict of {test_path: iterations}."""

    active_tests_file = tests_dir / "active_tests.txt"

    if not active_tests_file.exists():
        return {}

    active_tests: Dict[str, int] = {}

    try:
        with active_tests_file.open("r", encoding="utf-8") as handle:
            for line_num, line in enumerate(handle, 1):
                text = line.strip()
                if not text or text.startswith("#"):
                    continue
                parts = text.split()
                if len(parts) < 2:
                    continue
                test_path = parts[0]
                try:
                    iterations = int(parts[1])
                except ValueError:
                    continue
                if iterations < 1:
                    continue
                active_tests[test_path] = iterations
    except OSError:
        return {}

    return active_tests


def _read_cmake_cache(cache_path: Path) -> Dict[str, str]:
    entries: Dict[str, str] = {}
    try:
        with cache_path.open("r", encoding="utf-8", errors="ignore") as handle:
            for raw_line in handle:
                line = raw_line.strip()
                if not line or line.startswith(("//", "#")):
                    continue
                if "=" not in line or ":" not in line:
                    continue
                key_part, value = line.split("=", 1)
                key = key_part.split(":", 1)[0].strip()
                if not key:
                    continue
                entries[key] = value.strip()
    except OSError:
        return {}
    return entries


def _locate_cmake_cache(build_dir: Path) -> Optional[Path]:
    candidate = build_dir / "CMakeCache.txt"
    if candidate.exists():
        return candidate

    try:
        for path in build_dir.glob("**/CMakeCache.txt"):
            return path
    except OSError:
        return None

    return None


def _parse_cmake_set_file(file_path: Path) -> Dict[str, str]:
    entries: Dict[str, str] = {}
    try:
        import shlex
    except ImportError:
        shlex = None  # type: ignore[assignment]

    try:
        with file_path.open("r", encoding="utf-8", errors="ignore") as handle:
            for raw_line in handle:
                line = raw_line.strip()
                if not line.startswith("set(") or not line.endswith(")"):
                    continue
                inner = line[4:-1].strip()
                if not inner:
                    continue
                if shlex is not None:
                    try:
                        parts = shlex.split(inner, posix=True)
                    except ValueError:
                        parts = inner.split(None, 1)
                else:
                    parts = inner.split(None, 1)
                if not parts:
                    continue
                key = parts[0]
                value = ""
                if len(parts) > 1:
                    value = parts[1]
                if value.startswith('"') and value.endswith('"'):
                    value = value[1:-1]
                if value.startswith("'") and value.endswith("'"):
                    value = value[1:-1]
                entries[key] = value
    except OSError:
        return {}

    return entries


def _parse_compiler_metadata_from_cmake_files(build_dir: Path) -> Dict[str, str]:
    entries: Dict[str, str] = {}
    patterns = [
        "CMakeFiles/**/CMakeCXXCompiler.cmake",
        "CMakeFiles/**/CMakeCCompiler.cmake",
    ]
    for pattern in patterns:
        try:
            file_path = next(build_dir.glob(pattern), None)
        except OSError:
            file_path = None
        if not file_path:
            continue
        parsed = _parse_cmake_set_file(file_path)
        for key, value in parsed.items():
            entries.setdefault(key, value)
    return entries


def collect_compiler_metadata(build_dir: Path) -> Optional[Dict[str, Any]]:
    cache_entries: Dict[str, str] = {}
    cache_path = _locate_cmake_cache(build_dir)
    if cache_path:
        cache_entries = _read_cmake_cache(cache_path)

    compiler_file_entries = _parse_compiler_metadata_from_cmake_files(build_dir)
    if compiler_file_entries:
        combined_entries = dict(cache_entries)
        for key, value in compiler_file_entries.items():
            combined_entries.setdefault(key, value)
        cache_entries = combined_entries

    if not cache_entries:
        return None

    compiler = (
        cache_entries.get("CMAKE_CXX_COMPILER")
        or cache_entries.get("CMAKE_C_COMPILER")
        or ""
    )
    compiler_id = (
        cache_entries.get("CMAKE_CXX_COMPILER_ID")
        or cache_entries.get("CMAKE_C_COMPILER_ID")
        or ""
    )
    compiler_version = (
        cache_entries.get("CMAKE_CXX_COMPILER_VERSION")
        or cache_entries.get("CMAKE_C_COMPILER_VERSION")
        or ""
    )
    generator = cache_entries.get("CMAKE_GENERATOR", "")
    base_flags = cache_entries.get("CMAKE_CXX_FLAGS", "").strip()

    per_config_flags: Dict[str, str] = {}
    prefix = "CMAKE_CXX_FLAGS_"

    for key, value in cache_entries.items():
        if key.startswith(prefix):
            config_key = key[len(prefix) :].lower()
            per_config_flags[config_key] = value.strip()

    return {
        "compiler": compiler,
        "compiler_id": compiler_id,
        "compiler_version": compiler_version,
        "generator": generator,
        "base_flags": base_flags,
        "config_flags": per_config_flags,
        "cache_path": str(cache_path) if cache_path else None,
    }


def print_configuration_build_details(metadata: Optional[Dict[str, Any]], config_name: str) -> None:
    if not metadata:
        print("  Compiler: <unknown> (CMakeCache.txt not found)")
        print("  Compiler flags: <unknown>")
        return

    compiler_desc = metadata.get("compiler") or ""
    extras = []
    compiler_id = metadata.get("compiler_id")
    if compiler_id:
        extras.append(compiler_id)
    compiler_version = metadata.get("compiler_version")
    if compiler_version:
        extras.append(compiler_version)

    if extras and compiler_desc:
        compiler_desc = f"{compiler_desc} ({' '.join(extras)})"
    elif extras:
        compiler_desc = " ".join(extras)
    elif not compiler_desc:
        compiler_desc = "<unknown>"

    print(f"  Compiler: {compiler_desc}")

    base_flags = metadata.get("base_flags", "")
    config_flags = metadata.get("config_flags", {}).get(config_name.lower(), "")
    combined_flags = " ".join(part for part in (base_flags, config_flags) if part).strip()
    if not combined_flags:
        combined_flags = "<none>"
    print(f"  Compiler flags: {combined_flags}")
