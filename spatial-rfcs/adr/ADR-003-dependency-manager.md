# ADR-003 — Dependency manager

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

The C++ kernel links against Eigen 3.4, Protobuf, SQLite, nlohmann-json, and later third-party geometry libraries (COLMAP, Open3D, GTSAM, Ceres Solver, OpenCV, gsplat). Dependencies must be reproducible across developers and CI, license-checkable, and consistent on both Windows (MSVC 2022) and Linux (GCC 12). The M0 actual build dependencies are: eigen, protobuf, sqlite3, nlohmann-json, gtest.

## Decision

Conan 2 is the package manager. A committed conan.lock pins exact versions of every dependency and transitive dependency for reproducibility. Per-profile flexibility covers Windows (msvc2022-x64) and Linux (linux-gcc12). Conan hooks enforce license validation against the THIRD_PARTY.yml registry, which pre-registers all backends as status "planned" (COLMAP, OpenMVS, Open3D, GTSAM, Ceres Solver, KISS-ICP, VGGT, LingBot-Map, gsplat, Nerfstudio, FFmpeg, OpenCV, PROJ/GDAL, LASzip, libE57Format) and the actual M0 dependencies. CI validates every resolved dependency against THIRD_PARTY.yml as part of the dependency-registry-validation gate.

## Alternatives

- vcpkg: rejected. Weaker lockfile story, less flexible per-profile handling, weaker license-hook story, and harder internal package distribution.
- System packages or CMake FetchContent: rejected. Not reproducible across platforms; FetchContent lacks a hermetic lock and license gate.
- Plain find_package without a manager: rejected. No version pinning and no license governance.

## Consequences

- Positive: reproducible builds via conan.lock; clean Windows/Linux matrix; automated license validation; a path to distribute internal packages (adapters, engine plugins).
- Negative: Conan learning curve and profile maintenance; cache and network cost in CI; occasional friction with third-party recipes.
- Risks and mitigations: commit conan.lock and treat recipe patches as reviewable changes; keep THIRD_PARTY.yml as the single registry of record; document profile setup in the repository; validate the registry in CI on every change.

## References

- THIRD_PARTY.yml (dependency registry)
- ADR-001 (Monorepo strategy)
- ADR-011 (Process worker isolation)
