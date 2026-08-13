# ADR-022 — Toolchain Bootstrapping

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform must be reproducible: the same git commit on the same platform must produce a byte-identical toolchain from a developer machine and from CI. Both first-class platforms (ADR-017) need a defined, automated bootstrap so that a new engineer's first build matches CI exactly. The platform depends on C++20, CMake presets, Conan 2 with a lockfile, Python tooling for the SDK, and third-party deps (eigen, protobuf, sqlite3, nlohmann-json, gtest) that must be pinned. Without version pinning, toolchain drift produces bugs that are nearly impossible to reproduce.

## Decision

Bootstrap is a documented, scripted process per platform. Windows: Git for Windows, Visual Studio 2022 Build Tools (MSVC 17.x) with the C++ workload, CMake >= 3.28, Python 3.11, and Conan 2 installed via `pip`. Linux: CI images define gcc-12 and clang-16 with the same CMake/Python/Conan versions. Conan 2 profiles are committed per platform (host and build profiles, toolchain settings), and all dependency versions are pinned in `conan.lock`, which is committed to the repository. Builds use CMake presets (`CMakePresets.json`) that reference the Conan-generated toolchain; presets are the only supported way to configure, so ad hoc configuration drift is prevented. Eigen is declared and pinned like any other dependency (ADR-019). The M0 actual dependency set is exactly `eigen`, `protobuf`, `sqlite3`, `nlohmann-json`, `gtest`; anything else requires registry validation. Python SDK dependencies (for the tooling itself) are pinned via a lockfile too, and `mypy`/`ruff`/`pytest` run from that pinned environment. Reproducibility is verified in CI: the Ubuntu and Windows jobs bootstrap from the documented steps, so any divergence between documentation and reality fails the pipeline.

## Alternatives

- **System-installed dependencies, no lockfile:** rejected — guarantees drift between machines and CI, and makes bug reproduction unreliable.
- **vcpkg instead of Conan 2:** rejected — Conan 2's lockfile and profile model map better to the two-platform matrix and to the repository's already-defined Conan conventions.
- **Bundled/vendored source in-tree:** rejected — bloats the kernel, complicates licensing audit, and contradicts the third-party registry approach.

## Consequences

- Positive: a fresh checkout on either platform builds identically to CI; lockfile pinning makes dependency upgrades explicit and reviewable; presets remove configuration guesswork; dep-registry validation enforces the M0 dependency floor.
- Negative: bootstrap has several moving parts (Git, VS Build Tools, CMake, Python, Conan); Conan lockfiles need care when adding dependencies; a toolchain upgrade is a deliberate, reviewed event rather than an ad hoc one.
- Risks and mitigations: risk of lockfile rot or abandoned dependency versions — mitigated by scheduled dependency audits; risk of local tools shadowing the pinned ones — mitigated by presets invoking the Conan toolchain and by CI verifying bootstrap steps; risk of MSVC/gcc behavioral differences — mitigated by the CI matrix running both in Debug and Release (ADR-016).

## References

- `docs/architecture/storage-model.md`
- ADR-017 (cross-platform support)
- ADR-019 (Eigen adoption — pinned as a Conan dependency)
- ADR-016 (testing strategy — CI matrix and gates)
