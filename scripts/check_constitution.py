#!/usr/bin/env python3
"""Enforce the Constitution change-control rule for the Spatial Platform.

CONSTITUTION.md section 2 freezes a set of repo paths. Any modification of a
protected path must reference a ratified RFC (e.g. ``RFC-0042``) in the commit
or PR body. This gate is part of the ``validation`` CI job.

P0 behavior:
  * changed files are computed against ``--base`` via ``git diff``; if no
    ``--base`` is supplied, every file in the repository is considered.
  * if any protected path changed, the ``--rfc RFC-0000`` argument must be
    passed (the P0 mechanism). When ``--base`` is given and exactly one commit
    lies in ``base..HEAD``, an ``RFC-NNNN`` reference in that commit message is
    accepted as an alternative.
  * the governance repo must contain ``CONSTITUTION.md``.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

PROTECTED_PREFIXES = (
    "core/coordinates/",
    "core/geometry/",
    "core/scene/",
    "core/artifacts/",
    "core/plugin/",
    "adapters/interfaces/",
    "schemas/",
)

RFC_RE = re.compile(r"RFC-\d{4}")


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _norm_path(path: str) -> str:
    return path.replace("\\", "/").lstrip("./")


def _git(repo: str, *args: str) -> str:
    try:
        proc = subprocess.run(
            ["git", "-C", repo, *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        if proc.returncode != 0:
            return ""
        return proc.stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def _changed_vs_base(repo: str, base: str) -> list[str]:
    """Return repo-relative paths changed between ``base`` and the working tree."""
    paths: list[str] = []
    tracked = _git(repo, "diff", "--name-only", "--no-ext-diff", base)
    for line in tracked.splitlines():
        line = line.strip()
        if line:
            paths.append(line)
    untracked = _git(repo, "ls-files", "--others", "--exclude-standard")
    for line in untracked.splitlines():
        line = line.strip()
        if line:
            paths.append(line)
    return paths


def _all_files(repo: str) -> list[str]:
    paths: list[str] = []
    for root, dirs, files in os.walk(repo):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, repo)
            paths.append(_norm_path(rel))
    return paths


def _is_protected(path: str) -> bool:
    return any(path.startswith(prefix) for prefix in PROTECTED_PREFIXES)


def _single_commit_message(repo: str, base: str) -> str:
    """Return the commit message if exactly one commit lies in base..HEAD."""
    count = _git(repo, "rev-list", "--count", f"{base}..HEAD").strip()
    if count == "1":
        return _git(repo, "log", "-1", "--format=%B").strip()
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Enforce the Constitution change-control rule."
    )
    parser.add_argument(
        "--repo",
        default=_repo_root(),
        help="Path to the spatial-platform repository (default: this repo).",
    )
    parser.add_argument(
        "--rfcs",
        default=None,
        help="Path to the spatial-rfcs governance repository.",
    )
    parser.add_argument(
        "--base",
        default=None,
        help="Git ref to diff against (default: all files if omitted).",
    )
    parser.add_argument(
        "--rfc",
        default=None,
        help="Ratified RFC reference for this change, e.g. RFC-0042.",
    )
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    rfcs = os.path.abspath(args.rfcs) if args.rfcs else os.path.join(
        os.path.dirname(repo), "spatial-rfcs"
    )
    if not os.path.isdir(repo):
        print(f"ERROR: --repo directory not found: {repo}", file=sys.stderr)
        return 1

    constitution = os.path.join(rfcs, "CONSTITUTION.md")
    if not os.path.isfile(constitution):
        print(
            f"ERROR: CONSTITUTION.md not found in governance repo: {rfcs}",
            file=sys.stderr,
        )
        return 1

    if args.base:
        changed = _changed_vs_base(repo, args.base)
        if not changed and not _git(repo, "rev-parse", "--is-inside-work-tree").strip():
            print(
                "WARNING: git is unavailable; falling back to scanning the "
                "whole repository.",
                file=sys.stderr,
            )
            changed = _all_files(repo)
        source = f"changes vs {args.base}"
    else:
        changed = _all_files(repo)
        source = "all repository files (no --base given)"

    protected = sorted(path for path in changed if _is_protected(path))

    if not protected:
        print("constitution: PASS - no protected paths changed")
        print(f"  (scanned {source}, {len(changed)} file(s))")
        return 0

    print(f"constitution: {len(protected)} protected path(s) changed ({source}):")
    for path in protected:
        print(f"  - {path}")

    rfc_ref = None
    if args.rfc and RFC_RE.search(args.rfc):
        rfc_ref = args.rfc
    if rfc_ref is None and args.base:
        message = _single_commit_message(repo, args.base)
        match = RFC_RE.search(message or "")
        if match:
            rfc_ref = match.group(0)

    if rfc_ref is None:
        print(
            "ERROR: protected paths changed but no RFC reference supplied; "
            "re-run with --rfc RFC-0000 (CONSTITUTION.md section 2).",
            file=sys.stderr,
        )
        return 1

    print(f"constitution: PASS - change-control satisfied via {rfc_ref}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
