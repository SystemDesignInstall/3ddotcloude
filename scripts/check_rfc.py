#!/usr/bin/env python3
"""Validate the governance repository (spatial-rfcs).

Checks:
  * every ``adr/ADR-*.md`` matches ``ADR-\\d{3}-[a-z0-9-]+\\.md`` and contains
    ``## Context``, ``## Decision``, ``## Alternatives``, ``## Consequences``
    sections and a ``- **Status:**`` line;
  * every ``rfc/RFC-*.md`` matches ``RFC-\\d{4}`` naming;
  * the ADR index table in ``README.md`` lists ADR-001 .. ADR-038.

Templates (``ADR-TEMPLATE.md``, ``RFC-TEMPLATE.md``) are scaffolding, not
records, and are exempt from the naming rules.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

ADR_RE = re.compile(r"^ADR-\d{3}-[a-z0-9-]+\.md$")
RFC_RE = re.compile(r"^RFC-\d{4}-?[a-z0-9-]*\.md$")
REQUIRED_SECTIONS = ("Context", "Decision", "Alternatives", "Consequences")
STATUS_RE = re.compile(r"-\s*\*\*Status:\*\*")
INDEX_ROW_RE = re.compile(r"^\|\s*(ADR-\d{3})\s*\|")
EXPECTED_IDS = {f"ADR-{i:03d}" for i in range(1, 40)}
TEMPLATES = ("ADR-TEMPLATE.md", "RFC-TEMPLATE.md")


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _iter_md(directory: str) -> list[str]:
    if not os.path.isdir(directory):
        return []
    return sorted(
        name for name in os.listdir(directory) if name.lower().endswith(".md")
    )


def _fail(messages: list[str]) -> int:
    print("rfc: FAIL", file=sys.stderr)
    for msg in messages:
        print(f"  - {msg}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate the spatial-rfcs governance repository."
    )
    parser.add_argument(
        "--rfcs",
        default=None,
        help="Path to the spatial-rfcs repository (default: sibling of this repo).",
    )
    args = parser.parse_args()

    rfcs = os.path.abspath(args.rfcs) if args.rfcs else os.path.join(
        os.path.dirname(_repo_root()), "spatial-rfcs"
    )
    if not os.path.isdir(rfcs):
        print(f"ERROR: governance repo not found: {rfcs}", file=sys.stderr)
        return 1

    adr_dir = os.path.join(rfcs, "adr")
    rfc_dir = os.path.join(rfcs, "rfc")
    readme = os.path.join(rfcs, "README.md")

    errors: list[str] = []

    # --- ADR files ---------------------------------------------------------
    adr_files = [
        name for name in _iter_md(adr_dir)
        if name.startswith("ADR-") and name not in TEMPLATES
    ]
    if not adr_files:
        errors.append("no adr/ADR-*.md records found")
    for name in adr_files:
        path = os.path.join(adr_dir, name)
        if not ADR_RE.match(name):
            errors.append(
                f"{name}: filename must match ADR-\\d{{3}}-[a-z0-9-]+.md"
            )
            continue
        with open(path, encoding="utf-8", errors="replace") as fh:
            content = fh.read()
        for section in REQUIRED_SECTIONS:
            if not re.search(rf"^##\s+{section}\s*$", content, re.MULTILINE):
                errors.append(f"{name}: missing '## {section}' section")
        if not STATUS_RE.search(content):
            errors.append(f"{name}: missing '- **Status:**' line")

    # --- RFC files ---------------------------------------------------------
    rfc_files = [
        name for name in _iter_md(rfc_dir)
        if name.startswith("RFC-") and name not in TEMPLATES
    ]
    for name in rfc_files:
        if not RFC_RE.match(name):
            errors.append(f"{name}: filename must match RFC-\\d{{4}} naming")

    # --- README ADR index --------------------------------------------------
    indexed: set[str] = set()
    if os.path.isfile(readme):
        with open(readme, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                match = INDEX_ROW_RE.match(line.strip())
                if match:
                    indexed.add(match.group(1))
    missing = sorted(EXPECTED_IDS - indexed)
    if missing:
        errors.append(
            f"README.md ADR index is missing: {', '.join(missing)}"
        )

    if errors:
        return _fail(errors)

    print("rfc: PASS")
    print(f"  ADR records validated: {len(adr_files)}")
    print(f"  RFC records validated: {len(rfc_files)}")
    print("  README.md index lists ADR-001 .. ADR-038")
    return 0


if __name__ == "__main__":
    sys.exit(main())
