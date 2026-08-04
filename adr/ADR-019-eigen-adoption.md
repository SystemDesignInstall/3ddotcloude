# ADR-019 — Eigen Adoption

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform needs a linear algebra foundation for the geometry core: matrix/vector operations, quaternions, rigid transforms, and SVD/eigen-decomposition for optimization tasks. The third-party backends that matter most (COLMAP, GTSAM, Ceres, Open3D) already use or interoperate with Eigen, so sharing Eigen avoids conversions at adapter boundaries. Any choice must be header-only or trivially portable to both Windows MSVC and Linux gcc/clang (ADR-017), must not force a new build of vendored deps, and must not leak into public interfaces or business logic (ADR-018).

## Decision

Eigen 3.4 is adopted as the underlying linear algebra library, used exclusively inside `core/geometry/**` internals and adapters, wrapped by the strict domain types (ADR-018). Rationale: Eigen 3.4 is header-only, requires no runtime linkage, is battle-tested in exactly the photogrammetry/SLAM ecosystem we integrate with, and its types are the natural bridge to GTSAM factors, Ceres residuals, and Open3D geometry. Eigen is never exposed in public interfaces, the Plugin API (`core/plugin/**`), schemas, Protobuf messages, or the SDK: raw Eigen matrices must not cross a contract surface, and `check-domain-types` enforces this. ABI note: because Eigen is header-only and inlined, it introduces no ABI surface of its own; adapters that link COLMAP/GTSAM already built against Eigen share the same headers, so no binary conversion layer is needed at the adapter seam. Eigen is not in the M0 actual dependency set (`eigen`, `protobuf`, `sqlite3`, `nlohmann-json`, `gtest`) but is added as an M0 dependency since the strict types and geometry core require it; it is declared and pinned in `conan.lock` via the Conan profile (ADR-022).

## Alternatives

- **glm:** rejected — game-oriented, not a fit for photogrammetry solvers, weak integration with GTSAM/Ceres/COLMAP, and GLM conventions conflict with our column-vector/strict-type model.
- **Custom in-house linear algebra:** rejected — enormous cost for no advantage, and it would break the natural bridge to GTSAM and Ceres.
- **Blaze or other header-only competitors:** rejected — far smaller ecosystem overlap with the backends that must be supported.

## Consequences

- Positive: instant interop with COLMAP/GTSAM/Ceres/Open3D at adapter boundaries; header-only means no linkage or ABI risk across the two toolchains; wrapped types keep business logic clean; proven numerical quality (SVD, Jacobi, self-adjoint eigensolvers) for the geometry core.
- Negative: Eigen's template errors can be unreadable and slow compilation; its API invites raw usage that the project forbids outside internals; a dependency version must be pinned and audited.
- Risks and mitigations: risk of Eigen leaking into public interfaces — mitigated by `check-domain-types` and review; risk of Eigen version skew with vendored backends — mitigated by `conan.lock` pinning; risk of template error pain — mitigated by keeping Eigen confined to small, reviewed modules.

## References

- `docs/specifications/geometry-model.md`
- ADR-018 (strict types — where raw Eigen is permitted)
- ADR-022 (toolchain bootstrapping — Conan profiles and version pinning)
- ADR-017 (cross-platform support)
