#!/usr/bin/env python3
"""Validate the third-party dependency registry (THIRD_PARTY.yml).

ADRs 003 and 031 make THIRD_PARTY.yml the single registry of record for every
dependency and backend. This gate (part of the ``validation`` CI job) checks:

  * YAML parses (PyYAML when installed, otherwise a minimal stdlib fallback);
  * every dependency has the required fields and legal enum values;
  * every package listed in ``conanfile.txt`` is registered in THIRD_PARTY.yml;
  * every ``status: active`` dependency uses a permissive, allowlisted license.

Copyleft licenses (AGPL-3.0, LGPL-2.1, ...) on ``status: planned`` backends
are reported as warnings only: those backends are gated behind legal review and
the separate-adapter process before they may be activated. Dependencies with
``commercial_use: no`` are likewise warnings - they may only enter research
modules, never the commercial build.

Exit codes: 0 = pass, 1 = fail.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

try:
    import yaml  # type: ignore

    HAS_YAML = True
except ImportError:  # pragma: no cover - exercised only without PyYAML
    HAS_YAML = False
    yaml = None  # type: ignore

REQUIRED_FIELDS = (
    "name",
    "repository",
    "version",
    "code_license",
    "commercial_use",
    "redistribution",
    "status",
)

VALID_VALUES = {
    "commercial_use": {"yes", "no", "conditional"},
    "redistribution": {"yes", "no", "conditional"},
    "status": {"planned", "active", "removed"},
}

# Permissive licenses allowed in the kernel / commercial build. "Public Domain"
# is accepted as the public-domain dedication of SQLite (permissive-equivalent
# of CC0-1.0).
PERMISSIVE_LICENSES = {
    "MIT",
    "BSD-3-Clause",
    "BSD-2-Clause",
    "Apache-2.0",
    "MPL-2.0",
    "Boost-1.0",
    "Zlib",
    "BSD",
    "ISC",
    "CC0-1.0",
    "Public Domain",
}

CONAN_NAME_RE = re.compile(r"^\s*([A-Za-z0-9_.-]+)/(?:[^\s#]*)")


def _repo_root() -> str:
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _load_yaml(path: str) -> dict:
    """Load a registry file, preferring PyYAML with a minimal fallback."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    if HAS_YAML:
        data = yaml.safe_load(text)
        if not isinstance(data, dict):
            raise ValueError("top-level structure must be a YAML mapping")
        return data
    return _parse_flat_yaml(text)


def _parse_flat_yaml(text: str) -> dict:
    """Minimal YAML-subset fallback used only when PyYAML is unavailable.

    Supports a top-level mapping key whose value is a list of mapping items
    with single-line scalar values. This covers the registry files produced by
    this repository; the note fields are kept on one line to stay compatible.
    """
    root: dict = {}
    current_list: str | None = None
    item: dict[str, str] | None = None
    for raw in text.splitlines():
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip(" "))
        if indent == 0 and not stripped.startswith("-") and ":" in stripped:
            current_list = stripped.split(":", 1)[0].strip()
            root[current_list] = []
            item = None
            continue
        if current_list is not None and stripped.startswith("- "):
            item = {}
            root.setdefault(current_list, []).append(item)
            body = stripped[2:].strip()
            if ":" in body:
                key, value = body.split(":", 1)
                item[key.strip()] = value.strip()
            continue
        if indent > 0 and item is not None and ":" in stripped:
            key, value = stripped.split(":", 1)
            item[key.strip()] = value.strip()
    return root


def _conan_packages(path: str) -> list[str]:
    """Extract package names from a Conan 2 requirements file."""
    packages: list[str] = []
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            stripped = line.strip()
            if not stripped or stripped.startswith("#") or stripped.startswith("["):
                continue
            match = CONAN_NAME_RE.match(stripped)
            if match:
                packages.append(match.group(1))
    return packages


def _normalize(name: str) -> str:
    return name.strip().lower().replace("_", "-")


def _as_string(value) -> str:
    """Render a field value as text.

    PyYAML coerces bare ``yes``/``no`` (YAML 1.1 booleans) to ``True``/``False``;
    the stdlib fallback keeps them as strings. Normalize so both paths agree.
    """
    if isinstance(value, bool):
        return "yes" if value else "no"
    return str(value).strip()


def _warn(message: str) -> None:
    print(f"  warning: {message}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate THIRD_PARTY.yml and conanfile.txt."
    )
    parser.add_argument(
        "--repo",
        default=_repo_root(),
        help="Path to the spatial-platform repository (default: this repo).",
    )
    args = parser.parse_args()

    repo = os.path.abspath(args.repo)
    registry_path = os.path.join(repo, "THIRD_PARTY.yml")
    conan_path = os.path.join(repo, "conanfile.txt")

    errors: list[str] = []

    if not os.path.isfile(registry_path):
        print(f"ERROR: registry not found: {registry_path}", file=sys.stderr)
        return 1

    try:
        data = _load_yaml(registry_path)
    except Exception as exc:  # noqa: BLE001 - report any parse failure
        print(f"ERROR: THIRD_PARTY.yml failed to parse: {exc}", file=sys.stderr)
        return 1

    engine = "PyYAML" if HAS_YAML else "minimal stdlib fallback"
    dependencies = data.get("dependencies", [])
    if not isinstance(dependencies, list):
        print("ERROR: THIRD_PARTY.yml has no 'dependencies:' list", file=sys.stderr)
        return 1

    registered: dict[str, dict] = {}
    for entry in dependencies:
        if not isinstance(entry, dict):
            errors.append("dependency entry is not a mapping")
            continue
        name = str(entry.get("name", "")).strip()
        if not name:
            errors.append("dependency entry missing required field 'name'")
            continue
        registered[_normalize(name)] = entry

        for field in REQUIRED_FIELDS:
            if field not in entry or _as_string(entry[field]) == "":
                errors.append(f"{name}: missing required field '{field}'")

        for field, allowed in VALID_VALUES.items():
            value = _as_string(entry.get(field, "")).lower()
            if value and value not in allowed:
                errors.append(
                    f"{name}: field '{field}' must be one of "
                    f"{', '.join(sorted(allowed))}; got {value!r}"
                )

        status = _as_string(entry.get("status", "")).lower()
        license_name = _as_string(entry.get("code_license", ""))
        commercial = _as_string(entry.get("commercial_use", "")).lower()

        if status == "active":
            if license_name not in PERMISSIVE_LICENSES:
                errors.append(
                    f"{name}: active dependency license {license_name!r} is not "
                    "in the permissive allowlist"
                )
        elif status == "planned" and license_name and license_name not in PERMISSIVE_LICENSES:
            _warn(
                f"{name}: copyleft/other license {license_name!r} on a planned "
                "backend - legal review + separate-adapter process required "
                "before activation"
            )

        if commercial == "no":
            _warn(
                f"{name}: commercial_use=no - research modules only, must never "
                "enter the commercial build"
            )
        if commercial == "conditional":
            _warn(
                f"{name}: commercial_use=conditional - verify the license terms "
                "before any commercial use"
            )

    # conanfile.txt cross-check -------------------------------------------
    if os.path.isfile(conan_path):
        for package in _conan_packages(conan_path):
            if _normalize(package) not in registered:
                errors.append(
                    f"conanfile.txt package '{package}' is not registered in "
                    "THIRD_PARTY.yml"
                )
        print(f"  conanfile.txt packages checked: {len(_conan_packages(conan_path))}")
    else:
        print("  conanfile.txt not present; skipped package cross-check")

    if errors:
        print("dependencies: FAIL", file=sys.stderr)
        for message in errors:
            print(f"  - {message}", file=sys.stderr)
        return 1

    print("dependencies: PASS")
    print(f"  registry parsed with {engine}")
    print(f"  dependencies registered: {len(registered)}")
    print("  every conanfile.txt package is registered; active licenses permissive")
    return 0


if __name__ == "__main__":
    sys.exit(main())
