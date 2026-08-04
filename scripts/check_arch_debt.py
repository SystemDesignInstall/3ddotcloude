#!/usr/bin/env python3
"""Detect architecture-debt markers in the kernel and CLI surface.

CONSTITUTION section 4 requires Architecture Debt = 0 in ``core/**``,
``engine/**`` and ``schemas/**``. This check scans ``core/``, ``engine/``,
``schemas/`` and ``cli/`` for the markers ``TODO``, ``FIXME``, ``HACK`` and
``XXX`` (case-insensitive, word-boundary) in source files
``.cpp/.hpp/.h/.cc/.proto/.sql/.py/.json``.

Exempt paths: ``python/research/`` and ``benchmarks/experimental/``.

In P0 there is no code yet, so the check exits 0 with a note.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

MARKER_RE = re.compile(r"\b(?:todo|fixme|hack|xxx)\b", re.IGNORECASE)

SCAN_DIRS = ("core", "engine", "schemas", "cli")

EXTENSIONS = (".cpp", ".hpp", ".h", ".cc", ".proto", ".sql", ".py", ".json")

EXEMPT_PATHS = ("python/research/", "benchmarks/experimental/")


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Detect architecture-debt markers (TODO/FIXME/HACK/XXX)."
    )
    parser.add_argument(
        "--repo",
        default=_repo_root(),
        help="Path to the spatial-platform repository (default: this repo).",
    )
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    violations: list[tuple[str, int, str, str]] = []
    scanned = 0

    for scan_dir in SCAN_DIRS:
        root = os.path.join(repo, scan_dir)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if not d.startswith(".")]
            for filename in filenames:
                if not filename.lower().endswith(EXTENSIONS):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, filename), repo)
                rel = rel.replace("\\", "/")
                if any(rel.startswith(exempt) for exempt in EXEMPT_PATHS):
                    continue
                scanned += 1
                path = os.path.join(dirpath, filename)
                with open(path, encoding="utf-8", errors="replace") as fh:
                    for lineno, line in enumerate(fh, start=1):
                        for match in MARKER_RE.finditer(line):
                            violations.append((rel, lineno, match.group(0), line))

    if not scanned:
        print(
            "arch-debt: PASS - no source files found in core/engine/schemas/cli "
            "(P0 validation skeleton)."
        )
        return 0

    if violations:
        print("arch-debt: FAIL", file=sys.stderr)
        for rel, lineno, marker, line in violations:
            print(
                f"  {rel}:{lineno}: {marker} marker (kernel must be debt-free)",
                file=sys.stderr,
            )
            print(f"    {line.strip()}", file=sys.stderr)
        return 1

    print(
        f"arch-debt: PASS - no debt markers found "
        f"({scanned} file(s) scanned)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
