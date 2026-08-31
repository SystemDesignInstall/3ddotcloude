# P3-impl-6b Verification Report

**Status:** PASS
**Date:** 2026-08-30
**Scope:** Levenberg-Marquardt GTSAM Optimizer Adapter — Conversion Correctness, Factor Graph Construction, Optimization Execution, Result Extraction, Schema Contract

---

## 0. Objective

Per the authorization guardrail: **6b must prove not the existence of an optimizer, but the correctness of conversion between canonical math and GTSAM.** This report demonstrates that the P3↔GTSAM conversions are mathematically correct, that factor construction maps each canonical edge type to the correct GTSAM factor, that LM optimization executes and extracts results, and that output conforms to the canonical CAS schema contract.

---

## 1. Deliverables

| File | Type | Lines |
|------|------|-------|
| `adapters/gtsam/gtsam_optimizer_adapter.h` | Full adapter interface (`OptimizerOptions`, `AnchorPriorConfig`, `OptimizationInput`, `OptimizationOutput`, `optimize()`) | 66 |
| `adapters/gtsam/gtsam_optimizer_adapter.cpp` | Full implementation: conversions, factor graph, LM execution, result extraction, hash, provenance | ~450 |
| `schemas/json/optimization-result.schema.json` | Canonical optimization result CAS contract (B1) | 168 |
| `tests/unit/test_gtsam_adapter.cpp` | 29 tests (conversion, factors, optimization, edge cases, determinism, schema, boundary) | 1188 |
| `tests/CMakeLists.txt` | Added `SPATIAL_OPTIMIZATION_RESULT_SCHEMA_JSON` (+ pose-graph, loop-closure) macros | +4 lines |
| `adapters/gtsam/CMakeLists.txt` | Added `/WX-` to suppress GTSAM third-party header warnings | +1 line |
| `docs/architecture/P3-impl-6b-implementation-mapping.md` | Implementation mapping (approved prior to implementation) | 12 sections |

---

## 2. Test Results

| Configuration | Tests | Pass | Fail |
|---------------|-------|------|------|
| Debug (full ctest) | 537 | 537 | 0 |
| Release (full ctest) | 537 | 537 | 0 |

New tests added: **29** (GTSAM adapter tests) → total GTSAM test coverage 29 across 10 suites.

### New Test Breakdown

| Suite | Tests | What It Proves |
|-------|-------|----------------|
| `GtsamAdapterConversion` (7) | Quaternion identity round-trip; non-trivial 90°Z; 180°Z; 90°X; Pose3 direct map (no inversion); pure translation; non-trivial SPD information matrix | P3 (x,y,z,w) scalar-last ↔ GTSAM Rot3 (w,x,y,z) scalar-first is lossless across non-trivial rotations; T_trajectory_camera maps to Pose3 without accidental inversion |
| `GtsamAdapterFactor` (5) | PriorFactor<sup>1</sup>; BetweenFactor; loop-closure as Between; GPS as prior; IMU edge skipped | Each canonical edge type (prior, odometry, loop_closure, gps, imu_preintegration) maps to the correct GTSAM factor; unsupported types are logged & skipped (fail-closed) |
| `GtsamAdapterOptimization` (3) | Small graph; loop-closure drift correction; 5-node non-zero-translation chain | LM converges, error reduces, anchor keeps world-frame reference |
| `GtsamAdapterResult` (3) | Result extraction; frame identity; node ordering | Optimized nodes preserve frame_id UUID, timestamp_ns, and monotonic sequence_index |
| `GtsamAdapterEdgeCases` (4) | Empty graph; no edges; invalid node reference; disconnected graph | Fail-closed behavior: empty/invalid graphs report `failed` status without exception; disconnected components converge independently |
| `GtsamAdapterDeterminism` (2) | Deterministic configuration hash; provenance fields | Same inputs → same SHA-256 config hash; provenance fully populated (optimizer name/version, config hash, adapter version, backend JSON) |
| `GtsamAdapterAnchor` (2) | Anchor enabled; anchor disabled | Anchor prior (§B3) keeps node 0 fixed within tolerance; without anchor the relative pose is preserved |
| `GtsamAdapterSchema` (1) | Schema validation | `OptimizationResult` JSON conforms to `optimization-result.schema.json` required fields, status enum, provenance, and node structure |
| `GtsamAdapterBoundary` (1) | Architecture boundary | Compile+link of adapter is the only GTSAM surface (enforced by build system) |
| `GtsamAdapterPermutation` (1) | Tangent-space permutation via optimization | Diagonal information matrix produces expected rotation/translation independence (P3 `[v,ω]` ↔ GTSAM `[ω,v]`) |

<sup>1</sup> The GPS and Prior factors are both implemented as `PriorFactor<Pose3>` (not `PriorFactor<Point3>`), because GTSAM keys hold `Pose3` values. A `PriorFactor<Point3>` on a Pose3 key causes a type mismatch. Position-only GPS constraints are realized by a Pose3 prior whose translation is the GPS fix and whose rotation is taken from the trajectory estimate (rotation unconstrained by the position-only information blocks).

---

## 3. Mapping Verification (implementation mapping §1–§12)

| Mapping Section | Implementation | Test | Verdict |
|-----------------|----------------|------|---------|
| §1 Quaternion conv | `quaternionP3ToGtsam` / `quaternionGtsamToP3` — scalar-first/s-last | QuaternionRoundTrip, NonTrivial, 180Z, 90X | PASS |
| §2 Pose3 conv | `poseP3ToGtsam` / `poseGtsamToP3` — position + Rot3, no inversion | Pose3Direct, Pose3TranslationOnly | PASS |
| §3 Tangent-space perm | `kTangentPermutation[6] = {3,4,5,0,1,2}` in `permuteMatrix6x6` | TangentPermutationViaOptimization, NonTrivialInformationMatrix | PASS |
| §4 Information→noise | `noiseModel::Gaussian::Information` (no double inversion) with permutation | NonTrivialInformationMatrix | PASS |
| §5 Graph construction | `PriorFactor<Pose3>`, `BetweenFactor<Pose3>` | PriorFactorMapping, BetweenFactorMapping | PASS |
| §6 Loop closure | `loop_closure` → `BetweenFactor<Pose3>` | LoopClosureFactorMapping, LoopClosureCorrectsDrift | PASS |
| §7 GPS | `gps` → `PriorFactor<Pose3>` (position-only) | GpsFactorMapping | PASS |
| §8 IMU skip | `imu_preintegration` → logged, skipped | ImuEdgeSkipped | PASS |
| §9 LM execution | `LevenbergMarquardtOptimizer` with configurable params | SmallGraphOptimization, MultipleConnectedPoses | PASS |
| §10 Result extraction | `Marginals::marginalCovariance` → upper triangles; Pose3→P3 | ResultExtraction, FrameIdentityPreserved, NodeOrderingPreserved | PASS |
| §11 Hash/provenance | `Sha256Hex` deterministic config hash; uuid result_id; provenance JSON | DeterministicHash, ProvenanceFields | PASS |
| §12 Anchor prior (§B3) | configurable `information_scale` (default 1e6) applied to node 0 | AnchorPriorEnabled, AnchorPriorDisabled | PASS |

---

## 4. Authorization Guardrail Status

| Item | Status | Detail |
|------|--------|--------|
| **B1** `optimization-result.schema.json` | **AUTHORIZED & IMPLEMENTED** | Created at `schemas/json/optimization-result.schema.json`; validated by `GtsamAdapterSchema.SchemaValidation` |
| **B3** default anchor prior `1e6·I₆` | **AUTHORIZED as configurable default** | Implemented as `AnchorPriorConfig.information_scale` default `1e6`, NOT ratified as normative constant (documented in mapping §8) |

---

## 5. Core Boundary Audit

| Check | Result |
|-------|--------|
| GTSAM `#include` in `core/**` | **CLEAN** — zero GTSAM headers/tokens incl. in `core/` |
| GTSAM `#include` outside adapter | **CLEAN** — only `adapters/gtsam/gtsam_optimizer_adapter.cpp` includes `<gtsam/...>` headers; two other matches are the adapter's own header include |
| `gtsam::gtsam` link in project | **ISOLATED** — `PRIVATE gtsam::gtsam` only on `spatial_gtsam_adapter` (`adapters/gtsam/CMakeLists.txt:38`) |
| `spatial_core` links GTSAM | **NO** — core links only sqlite3, nlohmann_json, gtest, eigen, protobuf |
| GTSAM token in `core/` | **Comments/string only** — `optimization.h:25` `"gtsam"` enum value, `pose_graph.h:5` comment. No dependency. |
| Architecture boundary test | `GtsamAdapterBoundary.ArchitectureBoundary` PASS |

**Verdict:** `spatial_core` and all non-adapter subsystems are GTSAM-free. The adapter is the sole compile+link surface for GTSAM (matches 6a boundary invariant).

---

## 6. Mathematical Conversion Verification

The 6b objective is to prove conversion correctness, not optimizer existence. The tests prove:

1. **Quaternion ordering** is preserved across the boundary: P3 `(x,y,z,w)` scalar-last is converted to GTSAM `Rot3(w,x,y,z)` scalar-first and back losslessly for identity, 90°Z, 180°Z, and 90°X. Both `Rot3` (returned directly) and `toQuaternion()` (Eigen quaternion, `q.x(),q.y(),q.z(),q.w()`) are exercised.

2. **No accidental pose inversion**: `T_trajectory_camera` (world-from-body) maps directly to `Pose3(R, t)` without inversion; round-trips preserve translation and rotation exactly.

3. **Tangent-space permutation** `[v,ω] ↔ [ω,v]` is applied consistently to noise models and covariance extraction via the P6 permutation matrix, so a diagonal information matrix (tight position, loose rotation) yields the expected translation/rotation behavior. No double matrix inversion (Information → noiseModel is direct).

4. **Covariance extraction** re-permutes the GTSAM-ordered 6×6 marginal covariance back into P3 ordering and flattens the position/rotation 3×3 upper triangles row-major.

---

## 7. Findings & Risks

| # | Finding | Risk | Mitigation |
|---|---------|------|------------|
| 1 | **Runtime DLL discovery for GTSAM tests.** `spatial_gtsam_tests` links the static GTSAM library, whose transitive TBB dependency (`tbb12.dll`, `tbbmalloc.dll`) must be on `PATH` at runtime. Test machinery must set `PATH` to the Conan package cache `bin` dir (Release: `...\.conan2\p\onetb3084d812b30c1\p\bin`) before running ctest. Without it, ctest reports `0xc0000135` (DLL not found) only for this one test. | **Medium (operational)** | Documented here; Conan runtime env (`conanrun.bat`) exists and can be sourced. A future CI/test-harness step should inject it. |
| 2 | **GPS realized as Pose3 prior** rather than `PriorFactor<Point3>`. This is required by GTSAM's keyed-value type discipline; position-only information blocks constrain translation while rotation follows the trajectory prior. | Low | Documented in mapping and §2. Behavior validated by `GpsFactorMapping`. |
| 3 | **IMU / visual-inertial edges are skipped.** The adapter currently supports only `prior`, `odometry`, `loop_closure`, `lidar_odometry`, `gps`; IMU pre-integration is logged and skipped (fail-closed where no other factor exists). | Low | P3-impl-5 (loop closure) and IMU are deferred workstreams; adapter degrades gracefully. |
| 4 | **Anchor prior scale (1e6) is a configurable default, not normative.** | Low | Explicitly not ratified as normative constant (B3 guardrail); overridable per-run. |
| 5 | `created_at_ns` is currently set to `0` (deterministic output). A real wall-clock timestamp is not set to keep output reproducible. | Low | Intentional for determinism; can be wired to a clock when non-deterministic metadata is accepted. |

---

## 8. Verification Commands

```powershell
# Debug
cmake --build --preset default
ctest --test-dir build/default -C Debug          # 537/537 PASS

# Release (add Conan TBB bin to PATH for GTSAM runtime DLLs)
$env:PATH = "...\.conan2\p\onetb3084d812b30c1\p\bin;$env:PATH"
cmake --build --preset release
ctest --test-dir build/release -C Release        # 537/537 PASS
```

---

## 9. Verdict

**PASS.** The P3↔GTSAM conversions are mathematically correct and lossless; every canonical factor type maps to the correct GTSAM construction; LM optimization executes and extracts canonical results with post-optimization covariance; output validates against the B1 schema; the architecture boundary (GTSAM isolated to the adapter, core GTSAM-free) is maintained. All 537 tests pass in both Debug and Release.

**STOP — do not start P3-impl-6c.** Per authorization guardrails, a separate decision is required on whether to accept the GTSAM layer architecturally before proceeding further.
