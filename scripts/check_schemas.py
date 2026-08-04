#!/usr/bin/env python3
"""Validate schema contracts under ``schemas/``.

Schemas are cross-language contracts and are Constitution-protected. This gate
checks:

  * every ``.proto`` file in ``schemas/protobuf/`` starts with
    ``syntax = "proto3";``;
  * every ``.schema.json`` file in ``schemas/json/`` parses as JSON and
    contains ``$schema`` and ``type``;
  * ``schemas/database/schema.sql`` exists and contains ``schema_meta``;
  * every ``.sql`` migration under ``schemas/database/migrations/`` is named
    ``\\d{4}_*.sql``.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

PROTO3_RE = re.compile(r'^\s*syntax\s*=\s*"proto3";')
MIGRATION_RE = re.compile(r"^\d{4}_[\w.-]+\.sql$")


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _iter_files(directory: str) -> list[str]:
    if not os.path.isdir(directory):
        return []
    return sorted(
        name for name in os.listdir(directory)
        if os.path.isfile(os.path.join(directory, name))
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate schema contracts under schemas/."
    )
    parser.add_argument(
        "--repo",
        default=_repo_root(),
        help="Path to the spatial-platform repository (default: this repo).",
    )
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    protobuf_dir = os.path.join(repo, "schemas", "protobuf")
    json_dir = os.path.join(repo, "schemas", "json")
    database_dir = os.path.join(repo, "schemas", "database")
    migrations_dir = os.path.join(database_dir, "migrations")
    schema_sql = os.path.join(database_dir, "schema.sql")

    errors: list[str] = []

    # --- protobuf ----------------------------------------------------------
    for name in _iter_files(protobuf_dir):
        if not name.endswith(".proto"):
            continue
        path = os.path.join(protobuf_dir, name)
        with open(path, encoding="utf-8", errors="replace") as fh:
            content = fh.read()
        if not PROTO3_RE.search(content):
            errors.append(
                f"schemas/protobuf/{name}: must start with syntax = \"proto3\";"
            )

    # --- JSON Schema -------------------------------------------------------
    for name in _iter_files(json_dir):
        if not name.endswith(".schema.json"):
            continue
        path = os.path.join(json_dir, name)
        try:
            with open(path, encoding="utf-8", errors="replace") as fh:
                data = json.load(fh)
        except json.JSONDecodeError as exc:
            errors.append(f"schemas/json/{name}: invalid JSON ({exc})")
            continue
        if not isinstance(data, dict):
            errors.append(f"schemas/json/{name}: must be a JSON object")
            continue
        if "$schema" not in data:
            errors.append(f"schemas/json/{name}: missing '$schema' key")
        if "type" not in data:
            errors.append(f"schemas/json/{name}: missing 'type' key")

    # --- database SQL ------------------------------------------------------
    if not os.path.isfile(schema_sql):
        errors.append("schemas/database/schema.sql does not exist")
    else:
        with open(schema_sql, encoding="utf-8", errors="replace") as fh:
            if "schema_meta" not in fh.read():
                errors.append(
                    "schemas/database/schema.sql does not contain 'schema_meta'"
                )

    for name in _iter_files(migrations_dir):
        if not name.endswith(".sql"):
            continue
        if not MIGRATION_RE.match(name):
            errors.append(
                f"schemas/database/migrations/{name}: filename must match "
                r"NNNN_*.sql (4-digit prefix)"
            )

    if errors:
        print("schemas: FAIL", file=sys.stderr)
        for message in errors:
            print(f"  - {message}", file=sys.stderr)
        return 1

    print("schemas: PASS")
    print(
        f"  protobuf: {len(_iter_files(protobuf_dir))} file(s), "
        f"json: {len(_iter_files(json_dir))} file(s), "
        f"migrations: {len(_iter_files(migrations_dir))} file(s)"
    )
    print("  schemas/database/schema.sql present with schema_meta")
    return 0


if __name__ == "__main__":
    sys.exit(main())
