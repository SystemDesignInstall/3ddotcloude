# P3-impl-7b — Geometric Loop Closure Verification — Implementation Specification / Readiness

**Status:** READY TO IMPLEMENT **ONLY AFTER THIS SPECIFICATION IS ACCEPTED**
**Date:** 2026-08-31
**Depends on:** P3-7a (✅ ACCEPTED), P2.3 FeatureArtifact (✅), P3 canonical `LoopClosure` (✅),
6c `VerifyCandidate`/`BuildLoopClosureEdge` scaffolding (✅), GTSAM adapter boundary (✅)

> Grounded only in existing canonical types + the accepted architecture. **No new repository
> research. No AI. No TEASER++ / Open3D / ICP / LiDAR. Do not modify protected canonical contracts.**

---

## 0. Objective

`P3-impl-7a` answered: *"are these two frames visually similar?"*
`P3-impl-7b` answers: *"is there a physically and geometrically consistent spatial relationship
between them sufficient to form a Loop Closure Constraint?"*

**Fundamental invariant:**

> **`LoopClosureCandidate` ≠ Verified `LoopClosure`. A descriptor similarity score SHALL NEVER be
> sufficient for acceptance.**

```
LoopClosureCandidate
   ↓  FeatureArtifact resolution
Feature correspondences
   ↓  Robust geometric estimation
Inlier / outlier classification
   ↓  Relative pose (only when mathematically justified)
Quality assessment
   ↓
LoopClosure (verified)  —or—  rejected (both persisted for audit)
   ↓  P3-impl-7c (PoseGraph / GTSAM)
```

---

## 1. Architectural pipeline (target)

```text
FeatureArtifact
   ↓ P3-7a
LoopClosureCandidate
   ↓ P3-7b
Geometric Verification
   ├── REJECT -> LoopClosure(status=rejected) -> persisted
   └── ACCEPT -> metrics -> (metric pose only if justified) -> LoopClosure(status=accepted)
              → provenance → persisted
              → P3-7c / PoseGraph / GTSAM
```

7b SHALL NOT invoke GTSAM; it produces a **constraint/observation**, never a pose mutation.

---

## 2. Scope

**Implement only geometric verification.**

**DO NOT:** invoke GTSAM; modify trajectories; optimize poses; build the PoseGraph; perform
multi-session merge; LiDAR registration; ICP; TEASER++; Open3D; AI / neural descriptors; new
repository research; modify protected canonical contracts.

**DO NOT:** let `candidate_score → automatic acceptance`.

---

## 3. Inputs (map to existing canonical types)

| Input | Existing canonical type / source | Notes |
|-------|----------------------------------|-------|
| Candidate | `LoopClosureCandidate` (`core/trajectory/loop_closure.h`) | 7a output; carries `feature_match_score`, matcher, source/target frame, trajectory |
| Source / target frame identity | `LoopClosureCandidate.source_frame_id` / `.target_frame_id` | UUID strings → `Frame` bridge |
| FeatureArtifacts | `FeatureArtifact` via existing `FeatureSet` + `LoadFeatureDescriptors` (7a) → `MatchingFrameDescriptors` (keypoints + descriptors) | `FeaturePoint{x,y,size,angle,response}` gives 2D image coords |
| Camera calibration / model | existing calibration types **if populated** (ImageObservation.focal_prior_px is a prior, never authoritative, ADR-006) | optional prior; see §7 calibrated vs uncalibrated |
| Trajectory / pose info | `Trajectory` / `TrajectoryPoseNode` (existing) | optional prior for physical-plausibility checks |

**No native COLMAP / OpenCV / GTSAM objects enter Core.** External backend → Adapter → canonical
data only.

---

## 4. Correspondences (decision — the critical gap)

`LoopClosureCandidate` carries **only a scalar score**, not point pairs. Geometry needs
`(x1,y1) ↔ (x2,y2)` correspondences.

**Decision (per user review): DO NOT modify `LoopClosureCandidate`** to store correspondences — that
would be a needless new canonical surface. Instead:

```
LoopClosureCandidate
   ↓ resolve source + target FeatureArtifact
   ↓ invoke the existing deterministic matcher (7a contract)
   ↓ obtain internal correspondence set (ephemeral verification input)
   ↓ geometric verification
```

Correspondences are an **internal verification representation**, not a permanent canonical artifact:

```cpp
struct FeatureCorrespondence {              // internal, adapter/verification scope
  std::uint32_t source_index;               // index into source FeatureArtifact keypoints[]
  std::uint32_t target_index;               // index into target FeatureArtifact keypoints[]
  double descriptor_distance;               // from the matcher
};
```

The matcher adapter already computes nearest-neighbour pairs; cleanest is an **additive** method on
`LoopClosureFeatureMatcher` returning the pairs (O1), falling back to re-running the matcher (O2) if
the seam cannot be extended without touching an existing signature. Confirm the least-invasive seam
at implementation time; `LoopClosureCandidate` remains a retrieval artifact.

---

## 5. Stage A — Resolve candidate

Validate: candidate exists; source ≠ target; source/target frames exist; their FeatureArtifacts
exist and resolve to the declared frame IDs; temporal separation respected. On any failure:

```
status = REJECTED
reason = INPUT_INVALID
```

---

## 6. Stage B/C — correspondence reconstruction + filtering

Reconstruct `FeatureCorrespondence[]` from the two FeatureArtifacts, then filter deterministically:

```
all descriptor matches → valid index check → finite-value check →
duplicate-correspondence removal → optional ratio/distance filtering → geometric estimator
```

Matcher semantics are deterministic (7a D-DI-02): identical inputs → identical correspondences.

---

## 7. Stage D — geometric model: calibrated vs uncalibrated

**The single most important architectural rule for 7b:**

| Evidence available | Geometry | Can we emit a metric relative pose? |
|--------------------|----------|-------------------------------------|
| Intrinsics (calibrated) | **Essential** geometry + robust estimation → relative pose | **YES** (metric up to correct scale handling) |
| No intrinsics (uncalibrated) | **Fundamental** geometry → geometric consistency | **NO** — only epipolar-consistency verification. **Never** present as a metric / scaled relative pose. |
| 3D correspondences (reconstructed) already populated | 3D/3D or 2D/3D | **YES** (strongest) — not a dependency for the first 7b |

The user directive is explicit and MUST be honored:

> Do not invent metric scale. If only a fundamental matrix is available, do not suddenly write
> `translation = [1.2, 0.4, 3.7] meters`. Metric information must follow from available evidence.

Consequence for the first implementation: the **primary proof path is the calibrated/essential or a
metric-consistent model** with explicit knowledge of scale handling; uncalibrated runs may verify
**geometric consistency** but must keep the verified `LoopClosure` **without** a fabricated metric
transform / covariance.

---

## 8. Stage E — robust estimation

Deterministic robust estimator (RANSAC), parameterized — **never hard-coded in Core**:

```text
max_iterations
confidence
geometric_threshold
minimum_inliers
minimum_inlier_ratio
residual_threshold
```

---

## 9. Quality metrics (minimum exposed / evaluated)

- `correspondence_count`
- `inlier_count`
- `inlier_ratio`
- geometric / reprojection residual
- RMSE (where applicable), median error, max error
- transform validity (finite; rotation is a valid normalized unit quaternion; translation finite; no NaN/Inf)
- physical plausibility vs. available priors
- confidence
- provenance
- status

These map onto the existing canonical `LoopClosure` fields (status, inlier_ratio, inlier_count,
confidence, temporal/spatial separation) + `ReconstructionProvenance`. The residual/RMSE/median/max
error extend via provenance `backend_specific_json` or an **additive, non-protected** field only if a
protected field is not available — verify before adding any field.

---

## 10. Acceptance policy (configurable, evidence-based)

```
ACCEPT iff:
  candidate_valid
  AND correspondence_count >= configured_min
  AND geometric_model_estimated
  AND inlier_count    >= configured_min
  AND inlier_ratio    >= configured_min
  AND residual        <= configured_max
  AND transform_valid
  AND transform_physical
  AND all required evidence is available
else REJECT
```

All thresholds configurable via `LoopClosurePipelineOptions`-style options (extend it additively, do
not alter existing fields). A single `feature_match_score` SHALL NEVER cause acceptance.

---

## 11. Constraint output — and what 7b is allowed to write

Two existing canonical types are involved:

- **`LoopClosure`** (accepted | rejected) — the 7b output record: status, inlier metrics,
  confidence, temporal/spatial separation, candidate back-reference, provenance. **This type carries
  NO transform / covariance / information-matrix fields.**
- **`PoseGraphEdge`** — the type that *does* carry the geometric constraint
  (`relative_position_xyz`, `relative_rotation_xyzw`, `information_matrix_6x6`, `confidence`,
  `source`, `configuration_hash`). It is built at **7c** time by the existing 6c
  `BuildLoopClosureEdge` from an ACCEPTED closure.

**Therefore 7b structurally cannot carry an invented metric transform:** its output is the
`LoopClosure`. Metric pose + uncertainty are emitted into a `PoseGraphEdge` only in 7c, and only when
metric geometry was actually available in 7b (see §7, §12).

---

## 12. Uncertainty semantics — the `identity as fake uncertainty` guardrail

Mirror the anti-guardrail already present in the GTSAM adapter (`ValidateInformationMatrix` rejects
silently-identity matrices) and the architecture's `Confidence ≠ uncertainty` rule:

- **Confidence** (`LoopClosure.confidence`) is an acceptance/reliability measure in [0,1]; it is NOT
  a covariance.
- **Covariance / information matrix** (on `PoseGraphEdge`) is geometric uncertainty.

**Policy:**

```
metric evidence available
   → derive covariance from the estimator (e.g. from inlier-residual scatter / Jacobian)
   → information matrix = inverse (validated SPD; never silently identity)
unable to derive trustworthy uncertainty
   → follow the explicit uncertainty policy (large/absent uncertainty, or omit the metric edge),
     never silently insert identity/default as if it were a real estimate
```

If only fundamental (non-metric) geometry exists, **no metric pose / covariance is produced at all**
— no fabricated values.

---

## 13. Invariants preserved

- Loop closure = observation/constraint, **never** pose mutation; trajectory immutable.
- **Constraint ≠ Transform** — `LoopClosure` / `PoseGraphEdge` (constraint) and pose `Transform` are
  distinct.
- Global optimization separate (7c / GTSAM adapter).
- GTSAM behind the adapter boundary (7b has no GTSAM includes/uses).
- Core backend-free.

---

## 14. Backend boundary

Do **not** create `core/opencv/`, `core/ransac/`, `core/essential_matrix/`.

```text
core/
  loop_closure/
    geometric_verifier.h      (backend-independent contract: verify(candidate, features, options) -> LoopClosure)
    verification_options.h    (configurable thresholds)
    [feature_matcher.h, loop_closure_candidate_gen.h  (7a, reused)]

adapters/
  visual_geometry/            (classical geometric implementation: fundamental/essential + RANSAC)
```

Core: backend-independent verification contract + canonical result semantics + acceptance policy.
Adapter: backend-specific geometric implementation. No OpenCV/COLMAP/GTSAM/native types in Core.

**First implementation:** a minimal, deterministic, std-only classical geometric implementation
sufficient for the proof — no OpenCV requirement yet (same posture as 7a). OpenCV remains an
acceptable future adapter; COLMAP stays the primary classical photogrammetry backend. TEASER++ /
Open3D / ICP are **out of scope** (they target 3D/3D and registration, not visual 2D loop
verification).

---

## 15. Test matrix (13 required)

1. **Valid loop accepted** — same scene, known geometry → ACCEPTED, high inlier ratio, valid constraint.
2. **Visually similar but geometrically wrong → REJECTED** (high similarity, low geometric consistency). *Proves 7b adds a real layer.*
3. **Synthetic outliers** — e.g. 100 corr, 30 valid, 70 outliers → RANSAC recovers the true model.
4. **Insufficient inliers → REJECTED.**
5. **Low inlier ratio → REJECTED.**
6. **High residual → REJECTED.**
7. **Degenerate geometry → REJECTED** (insufficient evidence for a stable estimate).
8. **Invalid feature data (NaN / Inf / bad indices / empty / mismatched dims) → deterministic rejection.**
9. **Determinism** — identical FeatureArtifacts + candidate + config → identical result.
10. **Source artifacts immutable** — FeatureArtifact hash unchanged before/after (as in 6c).
11. **Accepted verification → valid `LoopClosure`** (status, metrics, provenance populated).
12. **Provenance links** constraint to its source candidate + FeatureArtifacts.
13. **Debug and Release both pass.**

### Golden safety test — `FalsePositiveCandidateRejectedByGeometry`
7a candidate = valid (high score) · 7b geometry = invalid → final = **REJECTED**. Demonstrates
`Candidate ≠ LoopClosure`.

### Golden success test — `VerifiedLoopProducesConstraint`
Candidate → correspondences → geometric model → RANSAC → relative pose → quality checks → canonical
accepted `LoopClosure`.

---

## 16. Reuse of the 6c scaffolding

6c's `VerifyCandidate` filled inliers from the synthetic score as a stand-in. 7b **replaces** that
stand-in with genuine geometric verification but **reuses the same contract and thresholds**
(`LoopClosurePipelineOptions`: `verify_min_inlier_ratio`, `verify_min_inlier_count`,
`verify_min_confidence`, `false_positive_spatial_tolerance_m`) and the same
`BuildLoopClosureEdge` for 7c. No protected field of `LoopClosure` is changed.

---

## 17. Deferred (not in 7b)

7c (verified `LoopClosure` → PoseGraph → GTSAM), 7a.1 (executor integration, already OPEN),
production descriptors, OpenCV/TEASER++/Open3D adoption, ORB-SLAM3/RTAB-Map, multi-session, LiDAR,
AI/neural.

---

## 18. Recommended implementation increments (5, each verified)

- **7b-1** Correspondence + geometry contract (resolve artifacts → correspondences → adapter seam).
- **7b-2** Classical geometric estimator (F/E + RANSAC → inliers).
- **7b-3** Relative pose + quality (residuals / RMSE / median / max, transform sanity).
- **7b-4** Constraint creation (accepted/rejected `LoopClosure` + provenance + CAS + metadata row).
- **7b-5** Integration tests (7a candidate → 7b verification → verified `LoopClosure`, incl. the two golden tests).

**Only after 7b-1..7b-5 pass in Debug and Release is 7b ACCEPTED.**

---

## 19. Open items for review before authorization

| # | Item | Recommendation |
|---|------|----------------|
| G1 | Correspondence seam (additive method vs re-run matcher) | additive method on `LoopClosureFeatureMatcher`; confirm no existing signature changed |
| G2 | Metric model for first proof | a calibrated/essential path with explicit scale handling; uncalibrated → consistency-only (no metric pose) |
| G3 | Placement of residual/RMSE metrics | extend provenance `backend_specific_json` (non-protected) or additive field if a protected field is unavailable |
| G4 | New capability stage vs orchestration function | orchestration function + declarative capability (same as 7a); executor integration remains 7a.1 |
| G5 | Confidence vs covariance separation | confidence on `LoopClosure`; covariance/information only on a 7c `PoseGraphEdge`, never fabricated |

---

## 20. Deliverable for this step

This document (`P3-impl-7b-implementation-readiness.md`).

**STOP after producing this report. Do NOT begin implementation until explicitly authorized.**
