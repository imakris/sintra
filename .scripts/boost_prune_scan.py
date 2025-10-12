#!/usr/bin/env python3
"""Compute reachable Boost headers starting from project includes."""
from __future__ import annotations

import argparse
import os
import re
from collections import deque

BOOST_INCLUDE_RE = re.compile(r"#\s*include\s*[<\"]boost/(.+?)[>\"]")
INCLUDE_RE = re.compile(r"#\s*include\s*(?P<delim>[<\"])(?P<path>[^\">]+)[>\"]")


def collect_project_includes(project_root: str) -> set[str]:
    """Find every `#include <boost/...>` directive outside the vendored tree."""
    includes: set[str] = set()
    for base, _, files in os.walk(project_root):
        if base.startswith(os.path.join(project_root, "third_party", "boost")):
            continue
        for name in files:
            if not name.endswith((".hpp", ".h", ".hh", ".ipp", ".cpp", ".cc")):
                continue
            path = os.path.join(base, name)
            try:
                with open(path, "r", encoding="utf-8", errors="ignore") as handle:
                    for match in BOOST_INCLUDE_RE.finditer(handle.read()):
                        includes.add(match.group(1))
            except OSError:
                pass
    return includes


def resolve_reachable_headers(boost_root: str, seeds: set[str]) -> set[str]:
    """Follow Boost includes recursively starting from the given seed headers."""
    reachable: set[str] = set()
    queue: deque[str] = deque(seeds)
    while queue:
        header = queue.popleft()
        if header in reachable:
            continue
        reachable.add(header)
        path = os.path.join(boost_root, header)
        if not os.path.exists(path):
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="ignore") as handle:
                contents = handle.read()
        except OSError:
            continue
        for match in INCLUDE_RE.finditer(contents):
            include_path = match.group("path").strip().replace("\\", "/")
            if include_path.startswith("boost/"):
                queue.append(include_path[len("boost/"):])
                continue

            if match.group("delim") == '"':
                current_dir = os.path.dirname(header)
                resolved = os.path.normpath(os.path.join(current_dir, include_path))
                if not os.path.isabs(resolved) and not resolved.startswith(".."):
                    queue.append(resolved)
    return reachable


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("project_root", nargs="?", default=".")
    parser.add_argument("--boost-root", default="third_party/boost/boost")
    parser.add_argument("--unused-output", default="boost_unused_headers.md")
    parser.add_argument("--used-output", default="boost_used_headers.md")
    args = parser.parse_args()

    seeds = collect_project_includes(args.project_root)
    reachable = resolve_reachable_headers(args.boost_root, seeds)

    boost_headers: set[str] = set()
    for base, _, files in os.walk(args.boost_root):
        for name in files:
            if name.endswith((".hpp", ".h", ".ipp", ".cpp")):
                rel = os.path.relpath(os.path.join(base, name), args.boost_root)
                boost_headers.add(rel)

    unused = sorted(boost_headers - reachable)
    used = sorted(reachable)

    with open(args.unused_output, "w", encoding="utf-8") as handle:
        handle.write("\n".join(unused))

    with open(args.used_output, "w", encoding="utf-8") as handle:
        handle.write("\n".join(used))

    print(f"Reachable headers: {len(used)}")
    print(f"Unused headers: {len(unused)}")


if __name__ == "__main__":
    main()
