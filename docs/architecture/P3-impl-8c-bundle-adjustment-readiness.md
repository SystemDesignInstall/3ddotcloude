# P3-impl-8c — Bundle Adjustment (Fixed Intrinsics) Readiness

**Status:** READINESS — 8c NOT implemented. 8a IMPLEMENTED (2026-09-05), 8b IMPLEMENTED (2026-09-06); both FROZEN and must not be modified by 8c.
**Dependency base:** this document resolves against the completed P3-impl-8 (8a/8b) deliverables (`core/geometry/camera_model.h`, `core/geometry/reprojection.h`, `core/geometry/triangulation.h`, `engine/pipeline/quality/quality_report.{h,cpp}`, `tests/unit/fixtures/closed_square_reconstruction.h`).
**Date:** 2026-09-06
**Scope:** Readiness-only implementation specification for the canonical Bundle Adjustment (photogrammetric refinement) increment: resolve 20 grounded investigation points, then define a design-only milestone `P3-impl-8c` producing an immutable Reconstruction revision **v4** from **v3**. **Zero code changes** in this document alone.
**Boundary:** Read-only investigation of the existing tree; every claim grounded in `file_path:line_number` citations. The base document `docs/architecture/P3-impl-8-reconstruction-consistency-readiness.md` (8-readiness) and its Addenda 8a/8b are the primary behavioral contracts.

---

## 0. Relation to existing documents

| Document | Relationship |
|----------|--------------|
| `P3-impl-8-reconstruction-consistency-readiness.md` | The governing spec for 8a/8b/8c/8d. §4.1 D1: BA AFTER `ApplyOptimizedTrajectory` (`:207-209`); §4.2 D2: in-repo canonical triangulation primary, COLMAP only an alternative adapter (`:211-213`); §4.3 D3: **FIXED intrinsics** for 8c — BA refines poses and 3D points ONLY (`:215-217`); §4.5 D5: inlier rule `r ≤ max(3·median, 2px)` and gate `RMS_after < 0.9·RMS_before`, enforced for 8c (`:230-239`); §4.6 D6: BA determinism = derived metrics/lineage + pinned `ColmapConfig.seed`, **not** raw backend byte identity (`:241-249`); §4.11 D: v4 lineage extends the PRESERVED/REPLACED/NEW/REJECTED classification (`:314-326`); §4.12 E: append-only `v2 → v3 → v4` chain, no in-place mutation (`:328-346`). Q10 (`:116-121`) already proposes the `core::ReconstructionOptimizer` seam this increment implements. Addendum 8b `:422` explicitly names 8c as the next increment and points at the seam shape. |
| `P3-impl-7c-implementation-readiness.md` | Defines `TrajectoryOptimizer` (`:59-63` of `core/trajectory/optimizer.h`) that the BA seam mirrors. |
| `P2.5-canonical-reconstruction-model.md` | `ReconPoint3D.error` = mean reprojection error px; track = synthesized multi-frame visibility. |
| Addendum 8b (`:387-422`) | Records the v3 production seam (`Retriangulate`), provenance, and the explicit hand-off: "no DB write, no supersede (that is the adapter's job in a later phase)" (`:400`). 8c takes that hand-off. |
| `RFC-0008` / `RFC-0009` | Adapter/capability boundary (COLMAP launched, never linked) and calibration-in-config prohibition. |

---

## 1. Current-state map (grounded, 8c-relevant slice)

### 1.1 The reusable 8a residual+gate layer (the metric source of truth)

- `core/geometry/reprojection.h` — the deterministic reprojection-consistency metric path, consumed exactly by 8c:
  - `ReprojectionObservation{image_id, point3d_id, keypoint_2d}` (`:41-45`) — the observation tuple BA consumes (2D pixel resolved from the FeatureArtifact chain; never stored inline, `:8-9`).
  - `InitializeReprojectionViews(rec)` (`:117-147`) — builds `CameraView{image_id, CameraModel, world_from_camera}` per image, ordered by `image_id`, fail-closed per camera via `CameraModel::FromReconCamera`.
  - `ComputeObservationResidual(view, xyz, keypoint)` (`:164-178`) — `r_ij = ‖keypoint − proj_i(p_R)‖` px; throws `ValidationError` on non-finite inputs (fail-closed).
  - `EvaluateReprojection(views, points, observations, residuals_out)` (`:186-354`) — pure, deterministic in caller order; median → D5 threshold (`ReprojectionThreshold` `:107-110`), inlier-only RMS/mean, per-image/per-point aggregates sorted by id, counts. **This computes RMS_before and RMS_after for the 8c gate on the SAME observation set.**
  - `ReprojectionGate{improvement_factor=0.9}` (`:365-367`) and `PassesReprojectionGate(rms_after, rms_before, 0.9)` (`:372-378`) — the reusable, frozen pure predicate; `false` for non-finite/negative inputs. Header comment (`:17-19`) states it exists specifically "for later 8b/8c orchestration".
- `engine/pipeline/quality/quality_report.h:35` — `engine::ReprojectionMetrics` is `using = spatial::core::geometry::ReprojectionMetrics` (single source of truth); the `reprojection` group is the additive RFC-0005 report channel (D4).
- `engine/pipeline/quality/quality_report.cpp:24-75` — deterministic-metric precedent: a `splitmix64` PRNG seeded from a SHA-256 digest of run identity, per RFC-0005 §2 (no wall-clock randomness). 8c derived-metric determinism (D6) inherits this discipline.

### 1.2 The 8b v3 seam it feeds (the hand-off 8c consumes)

- `core/geometry/triangulation.h` — `Retriangulate(v2, observations, options)` (`:269-571`), header-only, **no DB writes** (`:268`):
  - v3 = baseline copy of v2, fresh `reconstruction_id` (`:488`), `status="succeeded"` (`:489`), deterministic `created_at_ns=0` (`:490`).
  - Provenance: `backend.name="spatial_retriangulator"` v1.0.0 (`:494-496`); `input_artifact_hashes` = inherited + v2 id, sorted ascending, no self-reference (`:497-506`).
  - New points from `next_point_id=10001` (`:511`), never reusing v2 ids (PRESERVED keeps id, `:544-548`).
  - Lineage types `PointLineage`/`PointLineageEntry`/`RetriangulationLineageSummary` (`:73-93`) — the classification the v4 extension inherits (§4.11).
- `tests/unit/test_triangulation.cpp` — v3 semantics tested (`RevisionIsFreshAndNotSelfReferential` `:575-615`; `HighReprojectionErrorRejectsPoint` `:466-569`; fail-closed tests `:621-655`). These are the v3-side assertions 8c must preserve and extend.

### 1.3 The seam pattern 8c mirrors

- `core/trajectory/optimizer.h` — the canonical optimizer SEAM: header-only, "No optimizer (GTSAM/Ceres/custom) types appear here" (`:3-7`), `PoseOptimizationInput` (`:26-35`), `OptimizationTrace{converged, iterations, initial_error, final_error, error_reduction, status}` (`:38-45`), `PoseOptimizationOutput{optimized_nodes, trace, result_id}` (`:48-52`), abstract `class TrajectoryOptimizer { virtual PoseOptimizationOutput optimize(const PoseOptimizationInput&) = 0; }` (`:59-63`).
- `core/trajectory/optimization.h` — canonical optimization domain types: `OptimizationProvenance{OptimizerInfo name/version, configuration_hash, input_artifact_hashes, adapter_version, git_commit, backend_specific_json}` (`:23-36`), `OptimizedPoseNode` (`:42-51`), `OptimizationResult{status, iterations, initial_error, final_error, error_reduction, created_at_ns, provenance}` (`:55-67`). `OptimizerInfo.name` vocabulary (`:25`): `"gtsam","ceres","custom"` — an 8c BA backend reports as `"colmap"` (photogrammetric BA adapter), consistent with the matrix in 8-readiness Q9 (`:114`).
- `adapters/gtsam/gtsam_optimizer_adapter.h` — adapter boundary rules (`:6-14`): canonical types on the seam side, backend types below the adapter; `OptimizerOptions` (`:28-35`); `GtsamTrajectoryOptimizer final : public core::TrajectoryOptimizer` (`:72-116`) — the class pattern an 8c `ColmapReconstructionOptimizer` implements.
- `adapters/gtsam/gtsam_optimizer_adapter.cpp` — proven implementation conventions: `computeConfigurationHash` over sorted options/edges/nodes via Sha256 (`:132-173`), provenance `optimizer.name="gtsam"`, version, `backend_specific_json` with optimizer/iterations/status (`:175-201`), deterministic `created_at_ns=0` (`:457`), and the **fail semantics** — on failure `status="failed"`, output nodes = input (diagnostic) state that "must not be consumed as such by callers" (`:209-246`, `:332-361`). 8c encodes the same status discipline for BA.
- `core/trajectory/reconstruction_feedback.h` — `ApplyOptimizedTrajectory` (`:103-225`): the provenance-construction pattern 8c reproduces for v4 (`:144-180`: fresh `backend.name="spatial_optimizer"`, backend version adopted from the optimization result, `backend_specific_json` with optimizer/result_id/option, hashes inherit+append+sorted; deterministic `created_at_ns` reuse `:142`); `ValidateOptimizedReconstruction` (`:230-244`) — the structural validity check the v4 gate extends.

### 1.4 COLMAP adapter state (backend wiring gap)

- `adapters/colmap/colmap_adapter.cpp:37-41` — `kCapabilities = {"feature_extraction", "sparse_reconstruction", "bundle_adjustment"}`: **the `bundle_adjustment` capability is ALREADY declared** but unsupported by any executable stage.
- `adapters/colmap/colmap_config.h:30-34` — `ColmapStage` enumerates exactly three stages (`kFeatureExtractor`, `kMatcher`, `kMapper`); `ColmapConfig.seed` (`:81-82`) and `threads` (`:81`) are the determinism pins (D6 already names `:81-82`).
- `adapters/colmap/colmap_config.cpp` — `ColmapStageName`/`ColmapStageFromName` (`:118-141`), `TopLevelKeys` (`:33-39`) with `"seed"`, `IsKnownStage` (`:81-83`) accepting only the three stages, `FromJson` (`:149-293`) with calibration-vocabulary rejection (`RejectCalibrationVocabulary` `:88-104`, `CalibrationKeys` `:25-31`), `ToJson` (`:295-321`), `Plan` (`:323-341`), `BuildStageArgs` (`:343-376`). **There is no `bundle_adjuster` subcommand, option set, or plan step today.**
- `adapters/colmap/colmap_cli.cpp` — `StageSubcommand` maps only the three stages (`:29-39`); `BuildStageCommand` (`:58-82`) builds argv per stage; `StageArgTokens` (`:15-25`) already appends `--threads` and, when `config.seed` non-empty, `--random_seed <seed>` (`:20-23`) — **the seed plumbing D6 requires is already in place at the CLI builder level**; `DiscoverNativeModelFiles` (`:89-116`) requires exactly `cameras.bin`/`images.bin`/`points3D.bin`, fail-closed on partial models (`:106-114`).
- `adapters/colmap/colmap_adapter.h` — `ExecutionContext` (`:59-67`: workspace, ArtifactStore, input_refs/kinds, config_json, stage timeout, cancel token); `ColmapAdapter` (`:69-99`) with `executable="colmap"` default (`:76`, overridable by the probe shim) and `Execute(plan, sink)`; header `:8-12` — "launched, never linked"; **note: the `:12` comment "THIRD_PARTY.yml keeps COLMAP status `planned`" is stale** — `THIRD_PARTY.yml:84` shows COLMAP `status: active` (version 3.13, BSD-3-Clause, "launched as a subprocess, never linked").
- `adapters/interfaces/processing_adapter.h` — the sealed **Constitution-protected** adapter seam (`:55-81`, `adapters/interfaces/**`): `Descriptor`/`ValidateEnvironment`/`CreatePlan`/`Execute`; exceptions become typed `ProjectError` (ADR-014); **8c does NOT touch this header**.
- `adapters/colmap/colmap_converter.h` — the ONLY TU allowed to read COLMAP binary formats (native model files) and emit provisional JSON `schema_version 1`, deterministic via ADR-020; `colmap_converter.cpp` parses `SparseModel` (`:289`) → `SparseModelToReconstruction` (`:504`) with `ReconstructionToJson` (`:602`). **It is a reader only — there is no writer of native model files (cameras.bin/images.bin/points3D.bin) from a canonical Reconstruction anywhere.** This writer is the missing prerequisite for feeding v3 to `colmap bundle_adjuster`.
- `scripts/check_worker_boundary.py` — forbids `metadata_db.h`/`scene_query.h`/`sqlite3.h` includes and `sqlite3`/`MetadataDb`/`SceneQuery` identifiers under `engine/workers/**` and `adapters/colmap/**`: DB writes for 8c must live in the engine orchestration layer, never in the adapter/worker.

### 1.5 Persistence / revision semantics (no migration burden)

- `core/storage/metadata_db.h:186-193` — `ReconstructionRow{reconstruction_id, scene_id, coordinate_frame, status, created_at_ns, document_json}`; `status` vocabulary `"reconstructing"|"succeeded"|"failed"|"superseded"` (`:190`); `AddReconstruction` decl (`:450`).
- `core/storage/metadata_db.cpp` — `AddReconstruction` (`:1830-1861`, plain INSERT `:1837-1840`); `QueryLatestReconstructionByScene` (`:1863-1896`, `WHERE scene_id AND status='succeeded' ORDER BY created_at_ns DESC LIMIT 1`); `FindReconstructionsByScene` (`:1898-1930`, ascending created_at_ns); `SetReconstructionStatus` (`:1932-2003`) with the locked transition table (`:1970-1977`): `reconstructing→{succeeded,failed,superseded}`, `succeeded→superseded`, `failed/superseded` terminal. 8-readiness Q11/Q14 (`:123-124,140-141`) proves this is an append-only chain with **ZERO migrations needed** for 8a-8c.
- Revision tests: `tests/unit/test_reconstruction.cpp` (`:288-556` add/query/supersede semantics) and `tests/unit/test_reconstruction_feedback.cpp` (`:613-677` — insert-new + supersede-old then `QueryLatestReconstructionByScene` repoints).

### 1.6 Engine orchestration precedent

- `engine/pipeline/loop_closure_optimize_pipeline.h` — the stage pattern 8c mirrors: dependency-injected seam (`optimizer*` `:50`), "never #includes GTSAM or any optimizer backend" (`:15-17`), idempotent `Upsert*` persistence (`:21-26`), fail-closed when a dependency is missing (`:86-88`). An 8c orchestration stage follows the same discipline: inject `core::geometry::…ReconstructionOptimizer` (or the seam), persist via `AddReconstruction` + `SetReconstructionStatus`, never include the adapter.
- `engine/pipeline/pipeline_compiler.h` — capability-driven compilation (`Compile` `:32-36`); a reconstruction-BA pipeline stage resolves the `bundle_adjustment` capability already declared by `ColmapAdapter`.

### 1.7 Test infrastructure

- `tests/unit/fixtures/closed_square_reconstruction.h` — shared 8a/8b/8c fixture (`PoseDrift` `:39-42`, `ClosedSquareScene` `:44-60`, `BuildClosedSquareScene` `:125-317`, `BuildDefaultClosedSquareScene` `:319-328` with pinhole `fx=fy=500, cx=320, cy=240` 640×480, drift 0.15 m / 0.05 rad, 5 frames, 4 corners), `MakeReconstructionWithPoses` (`:330-343`). Keypoints = truth-projected pixels (`:217-246`) → under drifted poses the v1 rec has strictly positive RMS; after correction RMS → ~0, the clean basis for the 8c positive gate test.
- `tests/CMakeLists.txt` — the 8a/8b target pattern to copy: `spatial_reprojection_geometry_tests` (`:212-222`), `spatial_triangulation_tests` (`:226-236`); the gated GTSAM pattern (`:238-261`) shows how a real-backend test is conditionally built, but **8c's seam-level tests need no COLMAP install** — the seam + a deterministic in-repo reference exercise the gate and lineage (mirroring how `colmap_adapter.h:76` enables the "probe shim" to run adapter tests without COLMAP).

### 1.8 Gates

- `scripts/check_domain_types.py` — raw `Eigen::{Vector3d,Vector4d,MatrixXd,Matrix4d}` forbidden outside `core/geometry/` and `adapters/` (`:24-31`); 8a/8b held the count at the 2 pre-existing `core/trajectory/pose_graph_helpers.h` violations (**new code must add zero**). A header-only BA seam that consumes `ReprojectionObservation` (Eigen 2D) therefore belongs in `core/geometry/`.
- `scripts/check_constitution.py` — `PROTECTED_PREFIXES` (`:28-37`): `core/geometry/`, `engine/`, `schemas/`, `adapters/interfaces/` are protected (RFC-0002 referenced for 8a/8b); `core/trajectory/`, `core/reconstruction/`, `adapters/colmap/` are NOT.
- `scripts/check_worker_boundary.py` — see §1.4 (adapter/worker never touches DB).

---

## 2. Answers to the 20 investigation points

### P1 — Core seam: location and shape
**Answer:** `core::geometry` header-only seam `core/geometry/reconstruction_optimizer.h`, **mirroring `core/trajectory/optimizer.h`** (`:59-63`) and Q10 of the 8-readiness doc (`:116-121`):

```cpp
namespace spatial::core::geometry {
struct BundleAdjustmentInput {
  core::Reconstruction source;                    // v3, immutable read
  std::vector<ReprojectionObservation> observations; // resolved 8a observations
  double min_parallax_deg;                        // passthrough guard (re-triangulation reuse)
  /* fixed intrinsics policy: all ReconCamera.fx/fy/cx/cy/distortion treated constant */
  std::optional<std::string> random_seed;         // pinned (D6); nullopt fails closed
  int max_iterations;
};
struct BundleAdjustmentTrace {                    // mirrors OptimizationTrace
  bool converged; std::int64_t iterations;
  double rms_before_px; double rms_after_px; double mean_before_px; double mean_after_px;
  std::int64_t inlier_count_before; std::int64_t outlier_count_before;
  std::int64_t inlier_count_after;  std::int64_t outlier_count_after;
  double threshold_px_before; double threshold_px_after;
};
struct BundleAdjustmentResult {
  core::Reconstruction reconstruction;            // v4 (new ids, new status)
  BundleAdjustmentTrace trace;
};
class ReconstructionOptimizer {                   // abstract seam, engine-facing
 public:
  virtual ~ReconstructionOptimizer() = default;
  virtual BundleAdjustmentResult optimize(const BundleAdjustmentInput&) = 0;
};}
```
Rationale for `core/geometry` (not `core/trajectory` or `core/reconstruction`): the seam's canonical types ARE `ReprojectionObservation`/`CameraView` from `reprojection.h` (raw Eigen, allowed only in `core/geometry/` + `adapters/` per `check_domain_types.py:24-31`); matching the 8a/8b homes of `camera_model.h`/`reprojection.h`/`triangulation.h` keeps "no raw Eigen outside allowed dirs" at zero NEW violations; `core/geometry/**` is Constitution-protected, so the 8c commit carries `--rfc RFC-0002` exactly as Addenda 8a/8b did. The seam is header-only, canonical, pure: **no adapter type, no DB, no execution plan**.

### P2 — Adapter boundary
**Answer:** The back end is reached only behind the seam (engine rule, `loop_closure_optimize_pipeline.h:15-17`). COLMAP is launched, never linked (`colmap_adapter.h:8-12`; `THIRD_PARTY.yml:84`); the adapter (`adapters/colmap/*`) converts canonical ↔ COLMAP native types below the boundary (`colmap_adapter.h`, `colmap_converter.h`); the sealed `ProcessingAdapter` seam (`processing_adapter.h:55-81`) and the framed worker protocol (ADR-012) already exist and are NOT modified. 8c adds a `bundle_adjuster` stage to the COLMAP adapter surface (P16) and a `ColmapReconstructionOptimizer` implementing the seam; engine never includes COLMAP types.

### P3 — Canonical types
**Answer:** Input = `core::Reconstruction` (v3) + resolved `ReprojectionObservation[]` (`reprojection.h:41`) + `ReprojectionViews` from `InitializeReprojectionViews` (`reprojection.h:117`) + scalar options (P1). Output = `core::Reconstruction` (v4) + trace. The v4 document is a canonical `Reconstruction` (`reconstruction.h`), serialized via the existing `ReconstructionToJson` (deterministic, ADR-020) into `document_json` of the DB row (`metadata_db.h:192`); backend specifics live in `ReconstructionProvenance.backend_specific_json` (`reconstruction.h:103`) and the D5 metrics in the `QualityReport.reprojection` group (`quality_report.h:35`). Reuse `core::OptimizationResult`/`OptimizationProvenance` (`optimization.h:23-67`) as the optional report artifact shape.

### P4 — Pose parameterization / conventions
**Answer:** Firm: `T_reconstruction_camera` (world-from-camera) per §4.8 (`8-readiness:263-284`), scalar-last quaternion (`reconstruction.h:24`), matching `MakeReconPose` (`reconstruction_feedback.h:81-88`), `ApplyOptimizedTrajectory` (`:215-216`), and 8a/8b (`camera_model.h`, `reprojection.h:137-144`, `triangulation.h`). Adapter converts at the boundary to COLMAP's camera-to-world `(qvec_wxyz, tvec)` exactly as `colmap_trajectory_adapter.h:7-20` (`InvertColmapPose`) and `colmap_converter.cpp` already do — single inversion path, tested against the round-trip invariants of §4.8 (anti-double-inversion). Points live in the reconstruction frame; COLMAP's world frame coincides with the v3 reconstruction frame (identity alignment is declared, mirroring `reconstruction_feedback.h` CF-1).

### P5 — Fixed intrinsics (D3)
**Answer:** `fx fy cx cy distortion` are FIXED constants in 8c — read from `ReconCamera` (`reconstruction.h:32-43`) and passed verbatim into the native camera model on the way in and out. The adapter config surface pins every refine toggle that would move intrinsics to OFF (P9). Intrinsics DOF are deferred to 8d. 8-readiness D3 (`:215-217`) is the normative authority; §3.2 of the base doc explicitly forbids designing 8c intrinsics refinement (`:174`).

### P6 — Point optimization
**Answer:** BA refines image poses AND 3D points; 8c's output v4 carries refreshed `ReconPoint3D.xyz` (+ updated `error` = mean inlier reprojection px per `reconstruction.h:65`) plus refined `ReconImage.pose`. Points inherit 8b lineage semantics (§4.11): correspondence by observation track, PRESERVED keeps id / REPLACED gets a NEW id (`triangulation.h:544-554`), never mutating a v3 point in place. `track`/`point2d_idx` are carried unchanged (they identify the FeatureArtifact keypoints, not pixels).

### P7 — Residual construction via the 8a layer (no new math)
**Answer:** BA consumes the SAME deterministic residual definition as 8a/8b: `ComputeObservationResidual` → `r_ij = ‖keypoint_ij − proj_i(p_R_j)‖` (`reprojection.h:164-178`) with unit-ray / cheirality / per-model projection from `camera_model.h`. RMS/mean/median/threshold via `EvaluateReprojection` (`reprojection.h:186-354`) — the identical code path computes `RMS_before` (v3) and `RMS_after` (v4). No new reprojection code enters 8c. This satisfies the base-doc D2 mandate "reuse 8a projection layer" and keeps the 6 total math headers unified.

### P8 — Robust loss
**Answer:** Default robust kernel for the BA cost = **Soft-L1** (Huber-style), with a **deterministic scale pinned to the D5 threshold** (`max(3·median, 2 px)`, `reprojection.h:107-110`) so out-of-family outliers are down-weighted, never silently deleted. The robust-loss function and scale are config-surface algorithm settings (P9) constant per run; adapter maps them to COLMAP's bundle-adjuster loss parameters. A loss scale ≤ 0 or an unsupported kernel fails closed at config validation.

### P9 — Deterministic configuration and seed
**Answer:** Every 8c algorithm setting (intrinsics-fixed toggles, robust loss + scale, max iterations, threads) travels in `config_json` (`ColmapConfig`, `colmap_config.h`); `Sha256Hex(ToJson())` is the configuration hash (ADR-020 precedent `gtsam_optimizer_adapter.cpp:132-173`). **`random_seed` is REQUIRED for 8c: a `bundle_adjuster` plan without a non-empty `seed` is rejected at `ColmapConfig::FromJson` validation** (`colmap_config.cpp:149-293` precedent — mirror the existing strict-keys style; D6 `:241-249` mandates this). `StageArgTokens` already emits `--random_seed` when set (`colmap_cli.cpp:20-23`). Determinism of derived metrics/lineage follows the `quality_report.cpp:24-75` RNG discipline (RFC-0005 §2); raw backend output bytes are explicitly NOT bit-gated across platforms (D6).

### P10 — Before/after metrics
**Answer:** `EvaluateReprojection` over the SAME observation set yields, for v3 and v4 respectively: `rmse_px`, `mean_error_px`, `median_error_px`, `threshold_px`, `inlier/outlier/total_count` (all already in `ReprojectionMetrics`, `reprojection.h:94-104`). These flow into the `QualityReport.reprojection` group (additive RFC-0005, `quality_report.h:35`; D4 `8-readiness:219-228`) and `ReconstructionProvenance.backend_specific_json` (backend stats; D4 keeps the report as the canonical metric carrier). The 8c trace (P1) mirrors these fields so the seam is testable without the report layer.

### P11 — Acceptance gate (D5)
**Answer:** Frozen pure predicate reused verbatim: `PassesReprojectionGate(rms_after, rms_before, 0.9)` (`reprojection.h:372-378`) — strict `RMS_after < 0.9 · RMS_before`; non-finite/negative → `false`. Enforcement site = the engine orchestration stage (P16): **gate fails → NO v4 insert, NO supersede, run reported FAILED with both RMS values + outlier counts** (`8-readiness:236`). The gate never writes anything itself (`reprojection.h:17-19`).

### P12 — Fail-closed behavior
**Answer:** (i) target camera model outside the first-class set → `FromReconCamera`/`InitializeReprojectionViews` throws before any BA (`camera_model.h`, `reprojection.h:130-136`); (ii) non-finite injected keypoint → `ValidationError` (`reprojection.h:167-170`); (iii) incomplete `cameras.bin/images.bin/points3D.bin` after `bundle_adjuster` → fail closed via `DiscoverNativeModelFiles` (`colmap_cli.cpp:106-114`); (iv) BA subprocess non-zero exit / timeout → `AdapterError`, no output consumed; (v) gate failure → no v4 (P11); (vi) seam called with `random_seed == nullopt` → fail closed (P9); (vii) **no partial results**: an exception anywhere in the seam/adapter propagates as a typed `ProjectError` (ADR-014) and the engine persists nothing.

### P13 — v3 → v4 lineage
**Answer:** Exact mirror of `Retriangulate`'s provenance (`triangulation.h:483-507`) and `ApplyOptimizedTrajectory` (`reconstruction_feedback.h:144-180`): v4 = baseline copy of v3; fresh `reconstruction_id` (`FormatUuid(GenerateUuid())`); `status="succeeded"`; deterministic `created_at_ns=0`; `provenance.backend.name="spatial_bundle_adjuster"`, backend version + adapter version, `backend_specific_json` = `{"optimizer":"SoftL1","seed":"<pinned>","rms_before":…,"rms_after":…,"inlier/outlier counts":…,"iterations":…}`; `input_artifact_hashes` = inherited (v1→v2→v3 chain) + v3 id, sorted ascending, NO self-reference; PRESERVED/REPLACED/NEW/REJECTED counts (extend `RetriangulationLineageSummary`, `triangulation.h:88-93`, §4.11). No revision is renumbered or skipped.

### P14 — DB persistence and supersede ordering
**Answer:** Strictly ordered, matching the append-only chain (§4.12 `:328-346`) and the tested insertion pattern (`test_reconstruction_feedback.cpp:613-635`): (1) engine loads latest `succeeded` v3 (`QueryLatestReconstructionByScene`, `metadata_db.cpp:1863`); (2) run seam → v4; (3) compute gate ✓; (4) `AddReconstruction(v4_row)` (`metadata_db.cpp:1830`); (5) `SetReconstructionStatus(v3_id, "superseded")` (`metadata_db.cpp:1932`, valid transition `succeeded→superseded` `:1975`); (6) `QueryLatestReconstructionByScene` now repoints to v4 implicitly. Exactly one `succeeded` revision per scene per stage; a failed stage leaves v3 `succeeded` (no row, no supersede). The `SetReconstructionStatus` transition table already rejects any invalid ordering (`:1970-1977`).

### P15 — Backend choice: COLMAP vs Ceres
**Answer:** **COLMAP `bundle_adjuster`** (external subprocess), NOT Ceres — recommendation and full justification in the final delivery of this document. Key facts:
- COLMAP's `bundle_adjustment` capability is already declared (`colmap_adapter.cpp:37-41`); the adapter/seam/worker boundary, config surface with seed/threads, deterministic `StageArgTokens`, and fail-closed model discovery all exist and are COLMAP-native.
- `conanfile.txt` has NO Ceres; adding Ceres would require a new linked dependency + `THIRD_PARTY.yml` status change, breaking the "launched, never linked" posture (`colmap_adapter.h:8-12`, RFC-0008 §16). `THIRD_PARTY.yml:117-126` lists Ceres as `planned`; nothing in the tree wires it.
- GTSAM stays the global pose/constraint layer (architecture split, base-doc §3.2 `:168-174`). COLMAP/Ceres is the photogrammetric BA layer reached ONLY through the seam (Q9 `:113-114`).
- Required COLMAP-side deltas are additive: one new `ColmapStage::kBundleAdjuster`, the CLI subcommand mapping, an option set, and a **native-model WRITER** (see P16/§5) — the only genuinely new COLMAP code.

### P16 — Worker / executor integration
**Answer:** Adds a `bundle_adjuster` stage to the COLMAP path (same "plan input → materialize → run subcommand → discover output → canonical artifact" model as `colmap_adapter.cpp` today) plus:
- `colmap_config.{h,cpp}`: new `ColmapStage::kBundleAdjuster` (`colmap_config.h:30-34`) + `BundleAdjustmentOptions{loss_function, loss_scale_px, max_num_iterations, refine_focal_length=false, refine_principal_point=false, refine_extra_params=false, refine_extrinsics=true, refine_intrinsics=false}` + `TopLevelKeys`/`IsKnownStage`/`enabled_stages`/`BuildStageArgs`/`Plan` updates (`colmap_config.cpp:33-39,81-83,267-341,343-376`). **Intrinsics-fixed flags are pinned to OFF (P5).**
- `colmap_cli.{h,cpp}`: `StageSubcommand("bundle_adjuster")` (`:29-39`), `BuildStageCommand` for the BA stage (`--input_path <ws>/sparse/0`, `--output_path <ws>/sparse_ba`, + `StageArgTokens` incl. `--random_seed`), and model discovery under `sparse_ba/` reusing `DiscoverNativeModelFiles` (`:89-116`).
- `colmap_converter.{h,cpp}` (or a sibling `colmap_model_writer`): **the missing writer** — canonical v3 → `cameras.bin`/`images.bin`/`points3D.bin` (strict inverse of `ParseSparseModel`/`SparseModelToReconstruction`, `colmap_converter.cpp:289,504`), deterministic ordering (ADR-020), calibration round-tripped verbatim (P5). Restricted to the converter TU only (the sole binary-format reader rule, `colmap_converter.h`).
- New `adapters/colmap/colmap_bundle_adjustment_adapter.{h,cpp}` implementing `core::geometry::ReconstructionOptimizer` (P1) over the `bundle_adjuster` subcommand; used by tests and by the engine seam injection — engine/pipeline never includes it (P2/P11 pattern, `loop_closure_optimize_pipeline.h:15-17`).
- Engine orchestration stage `engine/pipeline/bundle_adjustment_optimize_pipeline.h` (protected path) mirroring `loop_closure_optimize_pipeline.h` (`:44-89`): pure inputs (v3 row + observations + seam + config + db), gate enforcement (P11), AddReconstruction/supersede ordering (P14), fail-closed when seam or db missing (`:86-88` style + trace). Worker boundary preserved: adapter/worker never touch the DB (`check_worker_boundary.py`).

### P17 — Migration 0009 — YES or NO
**Answer:** **NO.** Zero migrations for 8c (consistent with base-doc Q14 `:140-141` and D4 `:221-228`). The full lineage is already representable: append-only inserts (P14), the `status` supersede chain (`metadata_db.h:190`, transition table `metadata_db.cpp:1970-1977`), `created_at_ns + QueryLatestReconstructionByScene` repointing, and the provenance `input_artifact_hashes` chain (P13) — all on the existing `reconstructions` table columns (`metadata_db.h:186-193`, `0007_reconstruction.sql:9-11`). The suggested `optimized_at_ns` column is NOT needed: with exactly one `succeeded` revision per scene the ordering is unambiguous (`QueryLatest… ORDER BY created_at_ns DESC LIMIT 1` over one row), and BA timestamps are deterministic (`created_at_ns=0` policy, `triangulation.h:490`). If a future increment needs temporal audit the supersede `status` + provenance already deliver it without a schema change. `check_schemas.py` stays green.

### P18 — Positive test
**Answer:** `tests/unit/test_bundle_adjustment.cpp` (target `spatial_bundle_adjustment_tests`, mirroring `tests/CMakeLists.txt:212-236`): reuse `closed_square_reconstruction.h` — v3 is the fixture's DRIFTED reconstruction (points from `scene.rec`, observations = truth-projected keypoints). Run the seam with a deterministic real solver path. Assert: `trace.rms_after_px < 0.9 · trace.rms_before_px` (gate passes, `reprojection.h:372`); v4 `reconstruction_id ≠ v3`; points differ from v3 (`≥1 xyz` moved, mirroring `test_triangulation.cpp:423-460`); `inlier_count_after ≥ inlier_count_before`; provenance backend `"spatial_bundle_adjuster"`, hashes sorted, no self-reference (mirror `test_triangulation.cpp:575-615`); cameras/images intact (intrinsics identical — P5); unit-quaternion poses (`ValidateOptimizedReconstruction` `reconstruction_feedback.h:230-244`). A second test reuses the fixture with a stub seam whose `optimize()` returns the corrected geometry, asserting the P14 supersede ordering and `QueryLatest` repoint at the DB level (mirror `test_reconstruction.cpp:300-338`, `test_reconstruction_feedback.cpp:613-635`).

### P19 — Negative test
**Answer:** (a) **Gate failure → no revision:** feed a v3 that is already optimal (truth poses) plus a stub/real solver that returns no improvement; assert `PassesReprojectionGate` false and that no v4 row is added and v3 stays `succeeded` (mirror `test_triangulation.cpp:466-569` fail-closed tone). (b) **Fail-closed inputs:** unsupported camera model (`intrinsic_model="omnidirectional"`, mirror `test_triangulation.cpp:621-632`), non-finite keypoints (`:304-327`), missing observation refs. (c) **Config rejection:** a `bundle_adjuster` config with empty seed OR an intrinsics-refine flag ON → `ValidationError` (P9/P16; mirror `colmap_config.cpp` Violation style). (d) **Incomplete native model** → `AdapterError` via `DiscoverNativeModelFiles` (`colmap_cli.cpp:106-114`). (e) **Invalid supersede attempt** rejected by the transition table (`metadata_db.cpp:1978-1983`).

### P20 — Determinism test
**Answer:** Mirror D6 (`8-readiness:241-249`) and the `quality_report.cpp:24-75` discipline: run the full v3→v4 path TWICE with identical inputs and the SAME pinned seed; assert **derived** determinism — identical `trace.rms_before/after`, `mean`, `inlier/outlier` counts, identical lineage `input_artifact_hashes`, identical `backend_specific_json` and `QualityReport.reprojection` payload — and deterministic ordering (sorted ids). Explicitly DO NOT assert byte-identity of the raw COLMAP output across platforms (the documented non-gate). A seed-change test asserts a different seed → (in general) different refined geometry but the SAME derived-metrics schema and no effect on lineage hashes (which reflect inputs/identities, not floats).

---

## 3. Milestone definition

### 3.1 Scope (explicit)
- **8c — Bundle Adjustment (fixed intrinsics) → v4.** Deliverables (design paths only): the header-only canonical seam (P1) in `core/geometry/`; the COLMAP `bundle_adjuster` stage + native-model writer (P16/P15); the D5 gate enforcement orchestration force (P11/P14); tests P18-P20. All of 8a/8b stays byte-identical (frozen).
- **Directly reuses** (zero new math): `camera_model.h`, `reprojection.h` (residual + gate), `triangulation.h` (v3 seam + lineage semantics), `closed_square_reconstruction.h`, `quality_report.h:35` group, the metadata layer (P14), the CLI builder incl. `--random_seed` (`colmap_cli.cpp:20-23`).

### 3.2 NON-GOALS / hard constraints
- Intrinsics refinement (fx/fy/cx/cy/distortion DOF) is **8d, OUT OF SCOPE for 8c** (base-doc §3.2 `:174`).
- No dense reconstruction / multi-session / AI backends / SLAM / LiDAR / GNSS (base-doc §3.2 `:165-166`).
- GTSAM stays the global-optimization layer; photogrammetric BA is reached ONLY through the `ReconstructionOptimizer` seam (base-doc §3.2 `:168-174`); COLMAP never linked.
- v1/v2/v3 never edited; entire chain is append-only inserts + terminal supersede.
- No raw-Eigen growth outside `core/geometry/` + `adapters/` (keep `check_domain_types.py` at 2 pre-existing violations).
- No inline 2D coordinates in the reconstruction document (2D stays in FeatureArtifacts).
- Zero migrations (`check_schemas.py` green); `check_worker_boundary.py` green for `adapters/colmap/**`.

### 3.3 Phasing recommendation (kept from base D1)
`v1 → GTSAM → ApplyOptimizedTrajectory → v2 → re-triangulation (8b) → v3 → Bundle Adjustment (8c) → v4`. 8c is independently gated by P18-P20, mirrors the 8a/8b verification style (Debug + Release full suites + all `check_*.py`).

### 3.4 Invariants / Gates
Re-stated from 8-readiness §3.4 (`:189-197`) plus 8c-specific: gate `PassesReprojectionGate` (strict `0.9`), one succeeded revision per scene, provenance chain sorted/acyclic, pinned seed mandatory for the BA run, adapter never touches DB.

---

## 4. Decisions & resolutions

### 4.1 D-8c-1 — Seam home: `core/geometry/reconstruction_optimizer.h` (header-only) — **DECIDED (P1)**
Mirrors `core/trajectory/optimizer.h` (`:59-63`); lives alongside 8a/8b math; Constitution-protected path handled via `--rfc RFC-0002` like Addenda 8a/8b. Alternative homes (`core/trajectory/`) rejected because the seam's canonical observation type carries raw Eigen (`ReprojectionObservation.keypoint_2d`), which `check_domain_types.py` confines to `core/geometry/`+`adapters/`.

### 4.2 D-8c-2 — Backend: COLMAP `bundle_adjuster`, NOT Ceres — **DECIDED (P15)** (final recommendation in the delivery of this document).

### 4.3 D-8c-3 — Fixed intrinsics (D3) — **constrained**: every intrinsics-refine toggle pinned OFF; intrinsics are constants in and out (P5).

### 4.4 D-8c-4 — Robust loss default = Soft-L1, scale = D5 threshold — **DECIDED (P8)**; config-surface algorithm settings, constant per run.

### 4.5 D-8c-5 — No migration 0009 — **DECIDED (P17)**.

### 4.6 D-8c-6 — Derived-metric determinism + mandatory pinned seed (D6) — **DECIDED (P9/P20)**.

### 4.7 Normalized data flow / algebra (single source of truth, §4.8 of base extended)

| Quantity | Definition | Existing anchor |
|----------|-----------|-----------------|
| projection of point into image i | `u_i = proj_i(p_R)`, requires `w_C > 0` | `camera_model.h` `Project`; `reprojection.h:153-159` |
| per-observation residual | `r_ij = ‖keypoint_ij − proj_i(p_R_j)‖` px | `reprojection.h:164-178` |
| residual set | all track observations of all points | `EvaluateReprojection` `reprojection.h:186` |
| D5 inlier rule | inlier iff `r_ij ≤ max(3·median, 2 px)` | `reprojection.h:107-110` |
| BA cost (8c) | `min_{pose_i, p_R_j} Σ_j Σ_{i∈track(j)} ρ(r_ij · scale_c)` with `ρ = SoftL1`, intrinsics fixed, `scale = max(3·median, 2px)` | P5/P8 (new adapter is the only evaluator of this) |
| RMS (inlier-only) | `sqrt((1/N)·Σ r_ij²)` | `reprojection.h:94-104` |
| gate | `RMS_after < 0.9 · RMS_before` (strict, finite) | `reprojection.h:372-378` |
| pose convention | `T_rc = T_reconstruction_camera` world-from-camera, scalar-last quat | `reconstruction.h:24,49-54`; `reconstruction_feedback.h:215-216`; `reprojection.h:137-144` |
| coordinate transform (camera frame) | `p_C = Rᵀ(p_R − t)` | `camera_transform.h`; `reprojection.h:155-158` |

Invariants (tested): `proj/unproj` round trip per first-class model; unit quaternions post-BA (`reconstruction_feedback.h:238-242`); intrinsics byte-identical v3↔v4; `T_rc·T_cr = I`; gate strict-`<` boundary and non-finite rejection (`test_reprojection_geometry.cpp` precedent).

---

## 5. Proposed file / change manifest (paths ONLY — no code in this document)

| Change | Path | Protected? | Notes |
|--------|------|-----------|-------|
| NEW header-only seam + types (P1, P3, P10) | `core/geometry/reconstruction_optimizer.h` | yes (`core/geometry/`) | RFC-0002 |
| NEW native-model writer (v3 → cameras.bin/images.bin/points3D.bin) OR extend converter (P16) | `adapters/colmap/colmap_converter.{h,cpp}` (or sibling `colmap_model_writer.{h,cpp}`) | no | reader rule preserved |
| MOD COLMAP stage vocab + BA options + config validation (P16) | `adapters/colmap/colmap_config.{h,cpp}` | no | `TopLevelKeys`/`IsKnownStage`/`BuildStageArgs`/`Plan` |
| MOD CLI mapping + BA command + model discovery (P16) | `adapters/colmap/colmap_cli.{h,cpp}` | no | `--random_seed` reuse `:20-23` |
| NEW seam implementation over `bundle_adjuster` (P2, P15, P16) | `adapters/colmap/colmap_bundle_adjustment_adapter.{h,cpp}` | no | check_worker_boundary clean |
| NEW engine orchestration stage: gate + persistence ordering (P11, P14, P16) | `engine/pipeline/bundle_adjustment_optimize_pipeline.h` (and `.cpp` if needed) | yes (`engine/`) | RFC-0002 |
| NEW tests (P18-P20) + build registration | `tests/unit/test_bundle_adjustment.cpp`; `tests/CMakeLists.txt` (new `spatial_bundle_adjustment_tests`) | no | mirror `:212-236` |
| THIS READINESS DOCUMENT | `docs/architecture/P3-impl-8c-bundle-adjustment-readiness.md` | no | — |

Not touched (frozen): `core/reconstruction/reconstruction.h`, `core/geometry/{camera_model,reprojection,triangulation}.h` (8a/8b), `core/trajectory/*` seams, `engine/pipeline/quality/*`, `schemas/database/migrations/*`, `adapters/interfaces/processing_adapter.h` (sealed), `THIRD_PARTY.yml` (COLMAP already `active`; Ceres stays `planned`), `conanfile.txt` (no Ceres added).

---

## Addendum — READINESS ONLY

This document contains **zero code changes**. It records the grounded investigation and the design-only milestone for P3-impl-8c. Implementation, when approved, follows §2 P1-P20, §3, §4 and the §5 manifest, then this addendum is superseded exactly as Addenda 8a/8b were written in the base document.

*Prepared by opencode on 2026-09-06. All citations verified against the current tree (`P3-impl-8c-bundle-adjustment-readiness.md` is a new file; nothing else changed).*