# Third-Party Notices — Spatial Platform

This file records the third-party components used in the Spatial Platform and
the obligations that come with them. The authoritative machine-readable
registry is `THIRD_PARTY.yml` (enforced by the CI `check-dependencies` gate);
this document explains the process and the attribution requirements.

## Adding a dependency

1. Register it in `THIRD_PARTY.yml` **before** adding it to `conanfile.txt`.
   Every field is required: `name`, `repository`, `version`, `code_license`,
   `model_license`, `commercial_use`, `redistribution`, `status`, `notes`,
   `security_notes`. CI rejects any conan package that is not registered.
2. Set `status: active` only when the dependency actually enters the build.
   Backends that are approved direction but not yet built stay `planned`.
3. Run the gate locally: `python scripts/check_dependencies.py`. It fails on
   missing fields, unregistered packages, and non-permissive licenses on
   active dependencies.

## The commercial-use gate

- `status: active` dependencies must carry a permissive license from the
  allowlist (`MIT`, `BSD-2-Clause`, `BSD-3-Clause`, `Apache-2.0`, `MPL-2.0`,
  `Boost-1.0`, `Zlib`, `BSD`, `ISC`, `CC0-1.0`, `Public Domain`).
- Copyleft licenses (`AGPL-3.0`, `LGPL-2.1`, GPL variants) are **warnings** on
  `planned` backends: they may only enter the product through the
  separate-adapter process after legal review, never into the kernel
  (see `LICENSES/README.md`).
- `commercial_use: no` dependencies are research-only and must never link into
  a commercial build.
- Non-commercial AI models are never part of the commercial build
  (ADR-006; see `MODEL_LICENSES.yml`).

## Attribution requirements

When a component's license requires attribution (BSD, MIT, Apache-2.0, MPL-2.0,
Boost, CC-BY family, and public-domain dedications), its notice text is
reproduced under `LICENSES/` alongside the license text, and the component is
listed in the table below with its upstream repository. The platform's
end-user license appendices are generated from this document at release time.

## Active M0 dependencies

These components are linked into the M0 kernel build (ADR-031). Their exact
upstream notices are committed under `LICENSES/` at P1.

| Component | Version | License | Upstream | Purpose |
|---|---|---|---|---|
| Eigen | 3.4 | MPL-2.0 | https://gitlab.com/libeigen/eigen | Linear algebra internals (geometry layer only) |
| Protobuf | conan-tracked | Apache-2.0 | https://github.com/protocolbuffers/protobuf | Worker-protocol / schema serialization |
| SQLite3 | conan-tracked | Public Domain | https://www.sqlite.org/ | Metadata store (payloads stay in the artifact store) |
| nlohmann-json | conan-tracked | MIT | https://github.com/nlohmann/json | Manifest and metadata JSON |
| GoogleTest | conan-tracked | BSD-3-Clause | https://github.com/google/googletest | Kernel and integration tests |

> **Placeholder:** full notice texts, copyright lines, and the complete table
> of planned backends ship with P1, after the toolchain profiles are committed.
