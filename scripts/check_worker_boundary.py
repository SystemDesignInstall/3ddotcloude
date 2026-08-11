#!/usr/bin/env python3
"""Forbid direct database / scene access from the worker layer.

ADR-035 / ADR-038 (RFC-0007 §4-§6, read-boundary-review.md): capabilities
consume canonical scene data through the typed, read-only ``SceneQuery``
boundary, never through raw storage internals. Workers in ``engine/workers/**``
are the other side of the contract: they receive content hashes + CAS access
(``ArtifactStore``, ADR-010) and must never hold a database handle.

This gate makes that invariant machine-checkable. Forbidden inside
``engine/workers/**``:

- ``core/storage/metadata_db.h`` (direct include)
- ``core/scene/query/scene_query.h`` (direct include)
- ``<sqlite3.h>`` (direct include)
- the identifiers ``sqlite3``, ``MetadataDb``, ``SceneQuery``

Scope note: ``SceneQuery`` is forbidden specifically in *worker
implementations*. It remains the application/session read boundary and is
consumed by the CLI/session layer (RFC-0007 §8), not by workers. The CAS
(``artifact_store.h``) is the worker's legitimate persistence boundary.

Allowed extensions: ``.cpp``, ``.hpp``, ``.h``, ``.cc``.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# Direct include patterns -> the path a developer must route through instead.
FORBIDDEN_INCLUDES = (
    '"core/storage/metadata_db.h"',
    '"core/scene/query/scene_query.h"',
    "<sqlite3.h>",
)

# Bare identifiers that would leak the DB/scene layer into a worker.
FORBIDDEN_IDENTIFIERS = (
    "sqlite3",
    "MetadataDb",
    "SceneQuery",
)

SCAN_DIR = "engine/workers"

EXTENSIONS = (".cpp", ".hpp", ".h", ".cc")

RE_LEADING_WS = re.compile(r"^[ \t]*")

# Line-ending or word characters that can turn an identifier into a false
# positive (e.g. "MockSceneQuery"). Identifiers must not be a substring of a
# larger token.
RE_WORD = re.compile(r"[A-Za-z0-9_]")


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _contains_identifier(line: str, identifier: str) -> bool:
    """True when `identifier` appears in `line` not as part of a larger token."""
    start = 0
    while True:
        pos = line.find(identifier, start)
        if pos < 0:
            return False
        before_ok = pos == 0 or not RE_WORD.match(line[pos - 1])
        end = pos + len(identifier)
        after_ok = end == len(line) or not RE_WORD.match(line[end])
        if before_ok and after_ok:
            return True
        start = end


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that workers do not access DB / scene layers directly."
    )
    parser.add_argument(
        "--repo",
        default=_repo_root(),
        help="Path to the spatial-platform repository (default: this repo).",
    )
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    root = os.path.join(repo, SCAN_DIR)
    violations: list[tuple[str, int, str, str]] = []
    scanned = 0

    if not os.path.isdir(root):
        print(
            "worker-boundary: PASS - engine/workers not present "
            "(P0 validation skeleton)."
        )
        return 0

    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for filename in filenames:
            if not filename.lower().endswith(EXTENSIONS):
                continue
            rel = os.path.relpath(os.path.join(dirpath, filename), repo)
            rel = rel.replace("\\", "/")
            scanned += 1
            path = os.path.join(dirpath, filename)
            with open(path, encoding="utf-8", errors="replace") as fh:
                for lineno, line in enumerate(fh, start=1):
                    stripped = RE_LEADING_WS.sub("", line).rstrip()
                    for pattern in FORBIDDEN_INCLUDES:
                        if pattern in line:
                            violations.append(
                                (rel, lineno, f"include {pattern}", stripped)
                            )
                    for identifier in FORBIDDEN_IDENTIFIERS:
                        if _contains_identifier(line, identifier):
                            violations.append(
                                (rel, lineno, f"identifier {identifier}", stripped)
                            )

    if violations:
        print("worker-boundary: FAIL", file=sys.stderr)
        for rel, lineno, what, text in violations:
            print(
                f"  {rel}:{lineno}: forbidden {what} in worker implementation",
                file=sys.stderr,
            )
            print(f"    {text}", file=sys.stderr)
        print(
            "  Workers must not hold a database handle: consume content hashes "
            "and CAS (ArtifactStore) only. SceneQuery stays an "
            "application/session read boundary (RFC-0007 §8).",
            file=sys.stderr,
        )
        return 1

    print(
        f"worker-boundary: PASS - no direct DB/scene access from workers "
        f"({scanned} file(s) scanned)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
