# Boost Pruning Candidates

This report lists vendored Boost headers that are not reached from the set of Boost includes that the project currently uses.

## Methodology

1. Start from every `#include <boost/...>` directive found in the project's own headers and sources (excluding the vendored Boost directory).
2. Recursively follow `#include <boost/...>` directives inside the vendored Boost headers that are reachable from the starting set.
3. Record every Boost header that is visited by this traversal. Any Boost header under `third_party/boost/boost` that is not visited becomes a pruning candidate.

The helper script used for this traversal is located at `.scripts/boost_prune_scan.py` (see below).

## Summary of Unused Boost Headers

| Top-level directory | Candidate header count |
| ------------------- | ----------------------:|
| `mpl`               | 548 |
| `preprocessor`      | 71 |
| `config`            | 25 |
| `fusion`            | 1 |
| `intrusive`         | 1 |

A full, alphabetically sorted list of candidate headers is available in [`boost_unused_headers.md`](../boost_unused_headers.md).

> **Note**
> The script performs a textual include scan and may miss headers that are referenced indirectly via macros or platform-specific `#include` paths. Please double-check any removal before deleting files.

## Helper Script

```python
#!/usr/bin/env python3
"""Compute reachable Boost headers starting from project includes."""
from __future__ import annotations

import argparse
import os
import re
from collections import deque

INCLUDE_RE = re.compile(r"#\s*include\s*[<\"]boost/(.+?)[>\"]")


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
                    for match in INCLUDE_RE.finditer(handle.read()):
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
            queue.append(match.group(1))
    return reachable


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("project_root", default=".")
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
```

The script is ready to be re-run after code changes to refresh the lists above.
