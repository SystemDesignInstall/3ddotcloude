# P3-impl-7b — Verification Report

**Milestone:** P3-impl-7b — Geometric Loop Closure Verification
**Date:** 2026-09-01
**Verdict:** **PASS** (Debug 564/564 · Release 564/564)
**Status:** IMPLEMENTED — geometric verification replaces the 6c synthetic stand-in and produces
accepted/rejected `LoopClosure` records (do NOT auto-start 7c)

---

## 1. What was built

Deterministic, backend-independent **geometric loop-closure verification** on top of the 7a
candidate/FeatureArtifact pipeline, implementing the AUTHORIZED readiness spec
(`P3-impl-7b-implementation-readiness.md`, increments 7b-1..7b-5). A `LoopClosureCandidate`'s
descriptor similarity is **never sufficient** for acceptance: verified geometry decides.

| Layer | File | Purpose |
|-------|------|---------|
| Core seam (additive) | `core/loop_closure/feature_matcher.h` | `MatchingFrameDescriptors` gains `keypoints[]` (`FeatureKeypoint{x,y}`); `LoopClosureFeatureMatcher` gains additive virtual `MatchCorrespondences()` (base default = empty → fail closed; concrete matcher overrides). No existing signature changed. |
| Core contract | `core/loop_closure/geometric_verifier.h` | `LoopClosureVerificationInput` (candidate + resolved source/target `MatchingFrameDescriptors` + correspondences), `GeometricVerificationResult`, `GeometricVerifier` interface. |
| Core options | `core/loop_closure/verification_options.h` | `GeometricVerificationOptions`: `min_correspondences=20`, `min_inliers=8`, `min_inlier_ratio=0.7`, `max_residual=4.0`, `max_ransac_iterations=256`, `ransac_confidence=0.999`, `ransac_inlier_threshold=1.0`, `minimum_temporal_separation_ns`, `verify_min_confidence=0.6`. |
| Core reconstruction | `core/loop_closure/correspondence_reconstruction.h` | `ReconstructCorrespondences` — mats the two FeatureArtifacts via the matcher seam, validates indices, drops non-finite keypoints, dedupes; structural errors → `ProjectError(kValidationDomain)`. |
| Adapter | `adapters/visual_geometry/fundamental_verifier.{h,cpp}` | `FundamentalGeometricVerifier` — uncalibrated **Fundamental** geometry: 8-point + Hartley normalization, deterministic RANSAC (fixed seed `0x5EED7B12u`), Sampson-distance inlier test, rank-2 enforcement, refinement on inliers, quality gates, confidence clamp. `has_relative_pose` is **always false** (no intrinsics ⇒ no metric pose; evidence-based posture). |
| Adapter bound | `adapters/visual_matching/visual_matcher_adapter.h` | `L2NearestMatcher` overrides `MatchCorrespondences()` (reciprocal-NN pairs). |
| Engine orchestration | `engine/pipeline/loop_closure_verification.{h,cpp}` | `VerifyLoopClosureGeometry(...)` — resolves FeatureArtifacts (missing → typed `ProjectError`), `ReconstructCorrespondences`, runs the verifier, stamps `closure_id`/created-at/provenance, persists accepted **and** rejected `LoopClosureRow` via `MetadataDb`, + CAS `loop_closures` payload + manifest (input_artifact_hashes + configuration_hash). No pipeline-registry binding (same deliberate deferral as 7a → tracked debt). |
| Tests | `tests/unit/test_loop_closure_verification.cpp` | 15 tests (see §4) |

---

## 2. Test-count record

| Config | Before (7a) | After (7b) |
|--------|-------------|------------|
| Debug | 549/549 | **564/564** (+15) |
| Release | 549/549 | **564/564** (+15) |
| spatial_gtsam_tests | 39/39 | 39/39 (unchanged) |

The 15 new tests run under a new executable `spatial_loop_closure_verification_tests`. Debug and
Release full-suite ctest both 564/564; the GTSAM count is unchanged (7b has no GTSAM).

---

## 3. Deterministic geometric evidence

- **A. Valid geometry accepted.** Two views of the same planar target with real 2D keypoint
  patterns (excluding out-of-plane normals where F is degenerate) → F estimated, inlier ratio
  ≥ 0.7 → ACCEPTED, `confidence` in [0.6,1].
- **B. Outlier-contaminated consistent geometry accepted.** True model + 70% independent-model
  matches → RANSAC recovers the true model → inlier ratio above threshold → ACCEPTED.
- **C. Visually similar, geometrically wrong → REJECTED (golden).** Descriptor-similar points
  arranged incompatible with a single epipolar geometry → `LoopClosure(status="rejected")`.
  Proves the 7b layer is real: high `feature_match_score` ≠ acceptance.
- **D. False-positive candidate rejected by geometry (golden).** A 7a-style high-score candidate
  whose geometry fails → final `rejected`. Demonstrates `Candidate ≠ LoopClosure`.
- **E. Determinism.** Identical inputs → identical F, identical inliers, identical result
  (fixed RANSAC seed; no wall-clock).
- **F. Fail-closed typing.** Insufficient/malformed/degenerate input → `rejected` (configurable
  thresholds) or `ProjectError(kValidationDomain)` on structural errors (index mismatch, missing
  artifact, out-of-range correspondence).

---

## 4. What the 15 tests assert

- **FundamentalVerifierTest (10):** `ConsistentGeometryIsAccepted`; `OutlierContaminatedConsistentGeometryAccepted`
  (RANSAC robustness); `VisuallySimilarGeometricallyWrongRejected` (golden false-positive);
  `InsufficientCorrespondencesRejected`; `DegenerateCoincidentPointsRejected`; `DegenerateTargetCoincidentRejected`
  (normalization degeneracy → rejected — collinearity is NOT a rejection for an uncalibrated F, so the
  spec's "collinear" case is tested via the stronger coincident degeneracy);
  `BelowMinimumInlierRatioRejected`; `DeterministicForIdenticalInputs`;
  `MalformedInputThrows` / `OutOfRangeCorrespondenceThrows` (typed `ProjectError`).
- **ReconstructCorrespondencesTest (2):** `MatcherSeamProducesPairs` (L2 seam populates valid
  pairs); `CountMismatchThrows` (structural mismatch → `ProjectError(kValidationDomain)`).
- **LoopClosureVerificationTest (3):** `ValidLoopIsAcceptedAndPersisted` (row + CAS payload +
  manifest hashes/provenance); `FalsePositiveCandidateRejectedByGeometry` (golden);
  `MissingArtifactFailsClosedWithTypedError`.

---

## 5. Decisions honoured (from the readiness spec)

| Item | How |
|------|-----|
| G1 correspondence seam | **Additive** virtual `MatchCorrespondences()` on `LoopClosureFeatureMatcher` (base returns empty, fail-closed); no existing signature changed; `LoopClosureCandidate` still carries NO correspondence stores. |
| G2 metric model | FeatureArtifact has **no intrinsics** ⇒ **uncalibrated Fundamental-F verification**; `has_relative_pose` always false — **no metric pose/scale/covariance fabricated**. `LoopClosure` output carries no transform fields. |
| G3 residual/RMSE metrics | Fitted via provenance `backend_specific_json` on the manifest (non-protected); core `GeometricVerificationResult` carries summary quality (counts, ratio, residual, confidence) only. |
| G4 new stage vs orchestration | Orchestration **function** `VerifyLoopClosureGeometry` + no registry binding (identical posture to 7a's documented deferral; executor wiring remains the 7a.1 debt). |
| G5 confidence vs covariance | `confidence` (reliability, [0,1]) on `LoopClosure`; no covariance/information matrix anywhere in 7b (that belongs to a 7c `PoseGraphEdge` only). |
| Acceptance policy | Configurable thresholds (§1 options) decide ACCEPT/REJECT; `feature_match_score` never drives acceptance (tests C/D prove). |
| Failure semantics | Structural/data-validation failures → `ProjectError(kValidationDomain)`; geometric insufficiency → `LoopClosure(status="rejected")` with `rejection_reason`. Both accepted AND rejected closures persisted (D-LC-05 audit). |
| Determinism | Fixed RANSAC seed `0x5EED7B12u` (D-DI-02). |

---

## 6. Boundary / non-goals confirmation

- **GTSAM boundary unchanged:** 7b files (core/loop_closure verification contract + adapter,
  adapters/visual_geometry, engine/pipeline/loop_closure_verification) have **no GTSAM includes or
  uses** (grep-verified); `pose_graph_helpers.h`, the GTSAM adapter, and `spatial_gtsam_tests`
  (39/39) untouched.
- **No protected canonical contract changed:** all additions are new files or additive seams in
  already-7a-owned headers; `core/trajectory/pose_graph.h` / `LoopClosure` / schemas are untouched
  (persistence reuses the existing `loop_closure_candidates` table + `loop_closures`/manifest path;
  no DB schema change — loop-closure.schema.json `additionalProperties:false` ⇒ provenance rides the
  manifest).
- **Not implemented here:** PoseGraph assembly / GTSAM integration (7c), executor binding (7a.1
  debt), multi-session, TEASER++/Open3D/ICP, production descriptors, calibration-based essential
  geometry, AI/neural.

---

## 7. Governance gates (CONSTITUTION.md / mission gate list)

| Gate | Result |
|------|--------|
| `check_arch_debt.py` | PASS |
| `check_constitution.py --base fb1a333 --rfc RFC-0002` | PASS — 3 protected `engine/` paths changed, change-control satisfied via RFC-0002 (P3 traceability reference for loop-closure observations) |
| `check_dependencies.py` | PASS |
| `check_domain_types.py` | **FAIL — pre-existing 6c debt, NOT introduced by 7b.** The only flagged lines are
  `core/trajectory/pose_graph_helpers.h:48,69` (raw `Eigen::Vector3d` in core/trajectory/, a 6c
  committed helper). No 7b file is flagged (`core/loop_closure/`, `adapters/visual_geometry/`,
  `engine/pipeline/loop_closure_verification.cpp` all clear). The gate scans the whole repo with no
  `--base`, so it fails identically on the committed 7a baseline. Per the "fix only P3-impl-7b scope"
  rule (CONSTITUTION/Milestone policy), out-of-scope 6c debt is documented here and not modified in
  the 7b commit. |
| `check_rfc.py` | PASS — 39 ADRs, 8 RFCs validated |
| `check_schemas.py` | PASS |
| `check_worker_boundary.py` | PASS |

6/7 gates pass; the single failure (`check_domain_types`) is a pre-existing 6c debt and is
out-of-7b-scope, not a regression.

---

## 8. Files created/modified

**Created**
- `core/loop_closure/geometric_verifier.h`
- `core/loop_closure/verification_options.h`
- `core/loop_closure/correspondence_reconstruction.h`
- `adapters/visual_geometry/fundamental_verifier.h`, `.cpp`, `CMakeLists.txt`
- `engine/pipeline/loop_closure_verification.h`, `.cpp`
- `tests/unit/test_loop_closure_verification.cpp`
- `docs/architecture/P3-impl-7b-verification-report.md` (this file)

**Modified**
- `core/loop_closure/feature_matcher.h` (keypoints + `MatchCorrespondences` seam, additive)
- `core/loop_closure/loop_closure_candidate_gen.cpp` (`ParseFeaturePayload` captures keypoints)
- `adapters/visual_matching/visual_matcher_adapter.h` (`MatchCorrespondences` override)
- `adapters/CMakeLists.txt` (add `visual_geometry` subdir)
- `engine/CMakeLists.txt` (add `loop_closure_verification.cpp`)
- `tests/CMakeLists.txt` (add `spatial_loop_closure_verification_tests`)
- `docs/architecture/P3-impl-7b-implementation-readiness.md` (accepted spec — reference only)
- `docs/architecture/P3-milestone-status.md` (7b status + next = 7c)