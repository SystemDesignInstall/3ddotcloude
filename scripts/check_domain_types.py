#!/usr/bin/env python3
"""Forbid raw linear-algebra types in business-logic files.

CONSTITUTION principles 5 and 11 (ADR-007, ADR-018, ADR-019) require strict
domain types in business logic. Raw ``Eigen::Matrix4d``, ``Eigen::MatrixXd``,
``Eigen::Vector4d`` and ``Eigen::Vector3d`` are only permitted inside
``core/geometry/`` (math internals) and ``adapters/`` (backend boundary).

Scanned directories: ``core/``, ``engine/``, ``scene/``, ``coordinates/``
(when present). Allowed extensions: ``.cpp``, ``.hpp``, ``.h``, ``.cc``.

In P0 there is no C++ code yet, so the check exits 0 with a note.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

FORBIDDEN_PATTERNS = (
    "Eigen::Matrix4d",
    "Eigen::MatrixXd",
    "Eigen::Vector4d",
    "Eigen::Vector3d",
)

ALLOWED_DIRS = ("core/geometry/", "adapters/")

SCAN_DIRS = ("core", "engine", "scene", "coordinates")

EXTENSIONS = (".cpp", ".hpp", ".h", ".cc")

RE_LEADING_WS = re.compile(r"^[ \t]*")


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check for raw Eigen types in business-logic files."
    )
    parser.add_argument(
        "--repo",
        default=_repo_root(),
        help="Path to the spatial-platform repository (default: this repo).",
    )
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    violations: list[tuple[str, int, str]] = []
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
                if any(rel.startswith(allowed) for allowed in ALLOWED_DIRS):
                    continue
                scanned += 1
                path = os.path.join(dirpath, filename)
                with open(path, encoding="utf-8", errors="replace") as fh:
                    for lineno, line in enumerate(fh, start=1):
                        for pattern in FORBIDDEN_PATTERNS:
                            if pattern in line:
                                stripped = RE_LEADING_WS.sub("", line).rstrip()
                                violations.append((rel, lineno, pattern, stripped))

    if not scanned:
        print(
            "domain-types: PASS - no business-logic C++ source files found "
            "(P0 validation skeleton)."
        )
        return 0

    if violations:
        print("domain-types: FAIL", file=sys.stderr)
        for rel, lineno, pattern, text in violations:
            print(
                f"  {rel}:{lineno}: forbidden raw type {pattern!r} "
                f"outside allowed dirs (core/geometry/, adapters/)",
                file=sys.stderr,
            )
            print(f"    {text}", file=sys.stderr)
        return 1

    print(
        f"domain-types: PASS - no raw Eigen types outside allowed dirs "
        f"({scanned} file(s) scanned)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
