# License Policy — Spatial Platform

This directory is the human-readable summary of how the platform treats
third-party code, models, and datasets. The machine-readable registries it
refers to are enforced by CI:

- `THIRD_PARTY.yml` — code dependencies and backends (`check-dependencies` gate).
- `MODEL_LICENSES.yml` — AI model weights.
- `DATASET_LICENSES.yml` — validation and benchmark datasets.

## Policy

1. **Only permissive or conditional licenses live in the kernel.** The M0
   kernel links `eigen`, `protobuf`, `sqlite3`, `nlohmann-json`, and `gtest`
   (ADR-031). Any `status: active` dependency must carry a permissive license
   from the allowlist (`MIT`, `BSD-2-Clause`, `BSD-3-Clause`, `Apache-2.0`,
   `MPL-2.0`, `Boost-1.0`, `Zlib`, `BSD`, `ISC`, `CC0-1.0`, `Public Domain`).
   The `check-dependencies` gate fails otherwise.
2. **AGPL/GPL only via separate processes (adapters).** Copyleft code
   (COLMAP-family tools such as OpenMVS under AGPL-3.0, FFmpeg/LASzip under
   LGPL-2.1) is registered as `planned` and can only enter the product as an
   isolated adapter after legal review. Copyleft never links into the kernel
   or the core libraries (Constitution principle 9, ADR-013, ADR-031).
3. **Non-commercial models never enter the commercial build.** AI weights
   (VGGT, MASt3R, DUSt3R, ...) are priors only (ADR-006). Any model whose
   license restricts commercial use is confined to research; the commercial
   build contains no such model (see `MODEL_LICENSES.yml`).
4. **Every dependency is registered in `THIRD_PARTY.yml`.** The registry is the
   single source of record (ADR-003). A dependency that is not registered is
   rejected by CI; this applies to `conanfile.txt` packages and to backends
   referenced anywhere in the build.
5. **Datasets are registered before use.** A dataset used by tests or
   benchmarks must appear in `DATASET_LICENSES.yml` with its license,
   commercial-use terms, and checksum status. Customer-facing benchmark claims
   only come from datasets whose terms permit commercial reporting (ADR-029).

## Registration is required, not sufficient

Registration documents the decision to use a component; it does not grant
permission. New registrations, any `commercial_use: conditional` component, and
any planned copyleft backend go through Architecture Review and legal sign-off
before activation.

## Enforcement

The `validation` CI job runs `scripts/check_dependencies.py` on every change.
The registry files themselves are reviewed by the Architecture Board; edits to
`THIRD_PARTY.yml`, `MODEL_LICENSES.yml`, or `DATASET_LICENSES.yml` are part of
the dependency review process.
