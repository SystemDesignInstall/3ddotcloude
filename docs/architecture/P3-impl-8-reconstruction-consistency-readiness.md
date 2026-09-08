# P3-impl-8 — Reconstruction Consistency Readiness (Reprojection / Re-triangulation / Bundle Adjustment)

**Status:** INCREMENT 8b IMPLEMENTED (2026-09-06). INCREMENT 8a IMPLEMENTED (2026-09-05). 8c/8d NOT started — see Addenda 8a and 8b below.
**Decisions:** D1–D3 APPROVED; D4–D7 APPROVED-IMPLEMENTED for the parts 8a covers (D4 group + evaluator, D5 rule, D7 fixture), PROPOSED-CLOSED for the 8b/8c/8d parts (recomputed residuals, gate enforcement, BA seed, revision lineage).
**Date:** 2026-09-05
**Scope:** Milestone feasibility spec for closing the P3-impl-7 gap. After `ApplyOptimizedTrajectory` (P3-impl-7) updates only `ReconImage.pose`, the platform still keeps the OLD 3D points, old camera geometry, and old reprojection relationships. This document answers 15 grounded investigation questions, then defines a design-only milestone for (8a) reprojection/consistency model + metrics, (8b) re-triangulation producing an immutable new Reconstruction revision, (8c) bundle adjustment via a seam/adapter boundary.

**Boundary:** Read-only investigation of the existing repo. No external backends consumed as reference; the existing adapters (`adapters/colmap/*`, `adapters/gtsam/*`) are the only behavioral contracts. Everything below is grounded in `file_path:line_number` citations to the current tree.

---

## 0. Relation to existing documents

| Document | Relationship |
|----------|--------------|
| `P3-impl-7-phase3-e2e-orchestration-spec.md` | The completed E2E loop, which this milestone extends. Its §7 "Boundaries" explicitly defers "point transform / triangulation / bundle adjustment (Option A: camera poses only)" — P3-impl-8 targets exactly that deferral. |
| `P3-impl-7-verification-report.md` | Verifies `ApplyOptimizedTrajectory` (Option A) preserves points/cameras verbatim and defers "3D-point recomputation / re-triangulation / full BA" (§9, item 2). |
| `P2.5-canonical-reconstruction-model.md` | `ReconPoint3D.error` is "mean reprojection error (pixels)" (`:287`); `ReconPoint3D.track` is a synthesized multi-frame track, not a separate entity (`:335`). |
| `P3-impl-7c-implementation-readiness.md` | Defines the `TrajectoryOptimizer` seam this milestone's `ReconstructionOptimizer` would mirror. |
| `RFC-0008` / `RFC-0009` | The adapter/capability boundary and calibration-artifact contract. |

---

## 1. Current-state map (what exists today, grounded)

### 1.1 Canonical Reconstruction types (`core/reconstruction/reconstruction.h`)

- `ReconPose` (`:24`) — `rotation_xyzw` scalar-last, `translation_xyz`.
- `ReconCamera` (`:32`) — `camera_id`, `width`, `height`, `intrinsic_model` string, `fx/fy/cx/cy`, `distortion_model`, `distortion_coefficients`, optional `calibration_ref`. The intrinsic-model vocabulary is the string list at `:36`: `"pinhole", "opencv", "opencv_fisheye", "fov", "omnidirectional", "custom"`.
- `ReconImage` (`:49`) — `image_id`, `camera_id`, `frame_id` (UUID string → Frame), `name`, `pose` (T_reconstruction_camera), `detected`.
- `ReconPoint3D` (`:61`) — `point3d_id`, `xyz`, `color`, `error` (mean reprojection error px, `:65`), `TrackElement track` (`:66-70`: `image_id`, `point2d_idx`), `ReconUncertainty` optional.
- `ReconObservation` (`:79`) — the atomic 2D→3D link (`image_id`, `point2d_idx`); comment at `:76-77` states 2D coordinates live in the FeatureArtifact, not here.
- `Reconstruction` (`:118`) — root entity: `reconstruction_id`, `scene_id`, `session_ids`, `coordinate_frame`, `status`, `created_at_ns`, `provenance`, `cameras`, `images`, `points3D`. `status` vocabulary at `:123`: `reconstructing|succeeded|failed|superseded`.
- `ReconstructionProvenance.backend_specific_json` (`:103`) is the intended carrier for arbitrary backend metadata (D-CRM-11).

Key invariant relevant here: `ReconPoint3D.track` (`:71`) is the ONLY place a 3D point maps to multiple observation images. There is **no** inline 2D keypoint coordinate array in any reconstruction type — 2D coordinates are reachable only via `point2d_idx` → FeatureArtifact (see Q4/Q6/Q7).

### 1.2 Frame → ImageObservation → Reconstruction linkage

- `Frame` (`core/scene/frame.h:15`) — `frame_id`, `scene_id`, `session_id`, `timestamp_ns`, `sequence_index`, `sensor_id`, `pose_ref` (nil at import), `properties_json`.
- `ImageObservation` (`core/scene/observation_graph/image_observation.h:18`) — `observation_id`, `scene_id`, `sensor_id`, `frame_id` (kinematic frame, optional for stateless), `session_id`, `timestamp_ns`, `artifact_ref` (ImageArtifact UUID), `width/height/pixel_format`, optional `focal_prior_px` (ADR-006 prior, never authoritative).
- `SceneQuery` (`core/scene/query/scene_query.h`) — `Observations()`/`ObservationsByFrame`/`BySession`/`BySensor`/`InTimeRange` (`:62-70`); `ResolveCalibrationAt` (`:49-50`).
- The reconstruction→scene bridge is the `ReconImage.frame_id` string (`:52`). The schema file (`schemas/json/reconstruction.schema.json:172-176`) documents `frame_id → FeatureSet → FeatureArtifact` as the linkage: a reconstruction image bridges to the scene graph via its `frame_id`, from which per-frame keypoints/descriptors (the 2D observations) are resolvable. `ReconObservation`/`TrackElement.point2d_idx` (`reconstruction.h:68,81`) index into that FeatureArtifact's `keypoints[]`.
- The `frame_id` join is the same key `ApplyOptimizedTrajectory` uses (`reconstruction_feedback.h:199,211`), so re-triangulation can reuse the same frame_id-based joining.

### 1.3 Camera intrinsics path (full trace)

1. `Sensor` (`core/scene/sensor/sensor.h:19`) has a `calibration_id` latest-pointer; `Calibration` (`:34`) carries `intrinsics_json` (`:40`), `distortion_json`, `extrinsics_json`, `uncertainty_json`, and a half-open validity interval (`:44-47`).
2. `MetadataDb::ResolveCalibrationAt` (`core/storage/metadata_db.h:366`) resolves the `CalibrationRow` (`:169-181`) at a timestamp.
3. The session layer materializes it into a CAS `CalibrationArtifact` via `core/scene/query/calibration_materializer.h:38` (`MaterializeCalibrationArtifact`), which serializes through `core/artifacts/calibration_artifact.h:71` (`WriteCalibrationArtifact`).
4. The COLMAP adapter consumes that artifact (`adapters/colmap/colmap_adapter.h:43` input kind `"calibration"`; materialized into `calibration.json` in the workspace, `adapters/colmap/colmap_adapter.cpp:174,216`).
5. Canonical projection calib also deserializes from COLMAP native output: `colmap_converter.h` maps the COLMAP camera-model table (`:51-74`) into the calibration vocabulary, and `colmap_converter.cpp:536-549` fills `ReconCamera` (intrinsics + `DistortionModelForId` + `ExtractDistortion`).
6. **Supported intrinsic models** (two vocabularies must be reconciled):
   - Reconstruction canonical (`core/reconstruction/reconstruction.h:36`): `pinhole, opencv, opencv_fisheye, fov, omnidirectional, custom`.
   - Calibration artifact (`schemas/json/calibration.schema.json:25`): `opencv, pinhole, opencv_fisheye, omnidirectional, opengl, custom, fov` (the two are consistent; `fov` = COLMAP FOV-division, `omega`).
   - COLMAP native mapping (`colmap_converter.cpp:51-63`): SIMPLE_PINHOLE/PINHOLE→`pinhole`, SIMPLE_RADIAL/RADIAL/OPENCV→`opencv`, OPENCV_FISHEYE/SIMPLE_RADIAL_FISHEYE/RADIAL_FISHEYE→`opencv_fisheye`, FOV→`fov`, FULL_OPENCV/THIN_PRISM_FISHEYE→`custom`.

### 1.4 Where 2D keypoints (the reprojection observations) actually live

- `FeatureSet` (`core/scene/feature/feature_set.h:17-24`) — one per frame, `count == keypoints.length == descriptors.length`, `artifact_ref` → FeatureArtifact in CAS.
- The payload (`schemas/json/feature.schema.json:12-25`) stores each keypoint as `{x, y, size, angle, response}` in pixels — **the 2D observations needed for re-triangulation and reprojection-error computation already exist in the CAS**, keyed by frame → FeatureSet → FeatureArtifact. They are NOT inline in any Reconstruction type (confirmed by `reconstruction.h:76-77`).
- Image-space correspondence reconstruction for verification already exists in `core/loop_closure/correspondence_reconstruction.h` and `core/loop_closure/feature_matcher.h:26` (`FeatureKeypoint`).

### 1.5 COLMAP worker: external binary, stages captured

- The repo does **not** reimplement SfM; it **shells out** to the external `colmap` executable. `colmap_adapter.h:9-11` states "the backend is launched, never linked"; `ColmapAdapter` defaults `executable = "colmap"` (`:76`) and validates with a `--version` probe (`colmap_adapter.cpp:105-125`).
- Stage vocabulary is exactly three (`colmap_config.h:30-34`): `kFeatureExtractor`, `kMatcher`, `kMapper`. The plan default is `feature_extractor -> matcher -> mapper` (`colmap_config.h:29`).
- **There is NO `bundle_adjuster` stage invocable anywhere.** `colmap_cli.cpp:29-39` maps only `feature_extractor`, `exhaustive_matcher`, `mapper`; `colmap_adapter.cpp:245-249` accepts only those three. The `bundle_adjustment` string appears only in the declared capability list (`colmap_adapter.cpp:37-41`) and in docs — the capability is advertised but the CLI stage to serve it is absent.
- What is captured from each stage:
  - `feature_extractor`: writes `database.db` (`colmap_cli.cpp:65-68`).
  - `matcher`: fills `database.db` (`colmap_cli.cpp:72-73`).
  - `mapper`: writes `sparse/<n>/` (`colmap_cli.cpp:75-76`); output discovered at `<ws>/sparse/0/` (`colmap_cli.cpp:84-87`) requiring exactly `cameras.bin`, `images.bin`, `points3D.bin` (`colmap_cli.cpp:89-116`).
- **What is NOT captured:** per-point reprojection statistics beyond the scalar `error` (which is read verbatim from `points3D.bin`, `colmap_converter.cpp:249`), the image-space 2D point coordinates (parsed but **skipped** in `colmap_converter.cpp:220-222`), and any BA output stats (there is no BA stage). The `MapperOptions` (`colmap_config.h:67-73`) carry only `min_num_matches`, `ba_refine_principal_point`, `ba_min_num_residuals_for_multithreading` — no BA report/metric surface.

### 1.6 Geometry primitives that DO exist

- `core/geometry/se3.h:13` `SE3` — `TransformPoint`, `Inverse`, `operator*`, `ToMatrix`. Composition/rigid transform only; no projection.
- `core/geometry/camera_transform.h:15-45` — typed `WorldFromCamera`/`CameraFromWorld` wrappers around SE3 for extrinsic transforms. These are rigid transforms, **not** pinhole projection.
- `core/geometry/quaternion.h` — `Quaternion`, `Rotate`, `Normalized`.
- `core/geometry/direction_math.h` (added P3-7) — `RotateDirection` (`:20`), `Dot3` (`:28`), `Norm3` (`:34`). Vector/rotation math only.
- `core/trajectory/pose_graph_helpers.h` — `MakeCameraPose` (`:44-51`), `RelativePoseBetween` (`:64`), `MakeIsotropicInfo6` (`:116`), `MakeDeterministicLoopInfo6` (`:319`), `BuildMetricLoopClosureEdge` (`:379`).

**There is NO pinhole projection math anywhere in core/geometry.** `TransformPoint` (se3.h:46) moves a point rigidly; nothing divides through by camera focal length / projects 3D→2D or back-projects 2D→3D. `direction_math.h` stops at rotation/norm. This is a genuine greenfield for 8a/8b/8c.

---

## 2. Answers to the 15 investigation questions

### Q1 — Where do Reconstruction / ReconImage / ReconPoint3D / tracks / observations live?
All in `core/reconstruction/reconstruction.h` (see §1.1): `Reconstruction` `:118`, `ReconImage` `:49`, `ReconPoint3D` `:61`, `TrackElement` `:66-70`, `ReconCamera` `:32`, `ReconObservation` `:79`, `reconstruction_id` `:119` (UUIDv4 instance-scoped, D-CRM-07). Invariants: `ReconPoint3D.error` `:65` is mean reprojection error px (always present per schema `reconstruction.schema.json:215-219`); the track (`:71`) is the synthesized multi-frame visibility map; `ReconCamera.intrinsic_model` `:36` is a canonical vocabulary string. The reconstruction is persisted as metadata row + CAS JSON (`metadata_db.h:186-193` ReconstructionRow with `document_json`; `metadata_db.cpp:1830` AddReconstruction inserts the row; `colmap_converter.cpp:602` `ReconstructionToJson` with `schema_version: 2` `:605`).

### Q2 — How are Frame → ImageObservation → Reconstruction linked?
Via the `ReconImage.frame_id` UUID string (`reconstruction.h:52`) bridging to `Frame.frame_id` (`frame.h:16`). `Frame` ↔ `ImageObservation` share `frame_id` (`image_observation.h:22`) and `session_id`; `ImageObservation.artifact_ref` (`:25`) points at the ImageArtifact. The reconstruction image's 2D observations resolve `frame_id → FeatureSet → FeatureArtifact` (`reconstruction.schema.json:172-176`), and `TrackElement.point2d_idx` / `ReconObservation.point2d_idx` (`reconstruction.h:68,81`) index into that FeatureArtifact `keypoints[]`. `SceneQuery` reads observations by frame/session/sensor/time-range (`scene_query.h:62-70`). So the reconstruction is NOT directly joined to ImageObservation rows; the join is the shared `frame_id` string identity plus the `point2d_idx` into the per-frame keypoint artifact.

### Q3 — Where do intrinsics live / full trace / supported models?
See §1.3. Summary path: `Sensor` → `Calibration.intrinsics_json`/`distortion_json` (`sensor.h:34-47`) → `CalibrationRow` (`metadata_db.h:169-181`) resolved by `ResolveCalibrationAt` (`metadata_db.h:366`; `scene_query.h:49-50`) → materialized as `CalibrationArtifact` (`calibration_artifact.h:71`; `calibration_materializer.h:38`) → consumed by the COLMAP worker (`colmap_adapter.cpp:174,216`) → also deserialized directly from COLMAP output into `ReconCamera` (`colmap_converter.cpp:536-549`). Supported models: `pinhole, opencv, opencv_fisheye, fov, omnidirectional, custom` (`reconstruction.h:36`; consistency with `calibration.schema.json:25` and COLMAP table `colmap_converter.cpp:51-63`).

### Q4 — How are point tracks represented (multi-frame visibility)?
`ReconPoint3D.track` (`reconstruction.h:66-71`) is a `std::vector<TrackElement>`, each `{image_id, point2d_idx}`. A single 3D point maps to multiple `ReconImage` observations through repeated `TrackElement`s. **2D feature coordinates/reprojections are NOT stored in the reconstruction** — only the `point2d_idx` index into the FeatureArtifact (`reconstruction.h:68`; comment `:76-77`). The mean reprojection `error` (`:65`) is a scalar. `ReconUncertainty.source_count` (`:112`) records observation count but is optional. So multi-frame visibility is a synthesized track of indexes; actual pixel coords live in CAS FeatureArtifacts (Q1/Q2, `feature.schema.json:12-25`).

### Q5 — Which COLMAP results are usable as 3D points? What is / isn't captured?
The full native sparse model (`cameras.bin`/`images.bin`/`points3D.bin`) is parsed by `colmap_converter.cpp` and converted into `SparseModelPoint{point3d_id, xyz, rgb, error, track}` (`colmap_converter.h:140-148`), then into `ReconPoint3D` (`colmap_converter.cpp:579-593`). **Captured:** 3D xyz, color, per-point `error` (mean reprojection px from COLMAP), track `{image_id, point2d_idx}`, camera intrinsics+distortion, image poses. **NOT captured:** the image-space 2D point coordinates (explicitly skipped at `colmap_converter.cpp:220-222`), per-point reprojection statistics beyond the scalar error, any per-camera residual breakdown, and any BA output (no BA stage exists, Q8). The `point2d_idx` values ARE retained (`colmap_converter.cpp:586-591`), so the 2D coords remain reachable via the FeatureArtifact (resolve by frame/`point2d_idx`).

### Q6 — Does ANY triangulation helper exist?
**No.** A repo-wide case-insensitive search for `triangulat` finds only comments/deferred-notes and one `P3-impl-7c-implementation-readiness.md:263` reference to "triangulated points in front of both cameras" (a design note, not code). There is no `LinearizePoint`, no `Triangulation` function, no triangulation unit test. This is a confirmed greenfield for 8b. The indexing geometry needed as a prerequisite (SE3 invert/transform, `se3.h`) exists, but no intersection-of-rays / least-squares triangulation primitive does.

### Q7 — Does ANY reprojection code exist?
**No projection math exists.** Searches for `reproject|unproject|undistort|Normalize|bearing|Pixel` and `project` in headers return only: the `directions`-rotation helpers (`direction_math.h:20,28,34`), rigid transform (`se3.h:21` `TransformPoint`, `camera_transform.h:21,37` `TransformPoint`), and `quality_report.h` `ReprojectionMetrics` (`:28-31`) — the last being a metrics container, not a projector. `MakeCameraPose` (`pose_graph_helpers.h:44-51`) builds SE3 poses, not projections. `core/loop_closure/feature_matcher.h:26` `FeatureKeypoint` is a 2D pixel point type but there is no code that maps 3D→2D or 2D→3D through intrinsics. The word "reprojection" appears only in comments/specs and the `ReconPoint3D.error` field. **Conclusion: pinhole projection, back-projection, and distortion application are all absent and must be introduced (in `core/geometry/` to keep raw Eigen confined per `check_domain_types.py`).**

### Q8 — How is current COLMAP BA represented?
There is **no** BA invocation. The worker runs only `feature_extractor`, `matcher`, `mapper` (`colmap_adapter.cpp:245-249`; `colmap_cli.cpp:29-39`). The `bundle_adjustment` capability is advertised (`colmap_adapter.cpp:37-41`) but unsupported by any CLI stage — `ColmapStage` (`colmap_config.h:30-34`) enumerates three stages only. Any BA performed today is whatever COLMAP's `mapper` does internally (its own per-registration BA), and **no BA output statistics are captured** (the `MapperOptions.ba_refine_principal_point` / `ba_min_num_residuals_for_multithreading` at `colmap_config.h:67-73` are passed through, but no report/diagnostics are read back).

### Q9 — Can BA be wrapped with the EXISTING adapter boundary?
Yes, cleanly. The established seam pattern is: `core/trajectory/optimizer.h` defines the canonical `TrajectoryOptimizer` interface + `PoseOptimizationInput/Output/OptimizationTrace` (`:26,38,48,59`); `adapters/gtsam/gtsam_optimizer_adapter.h` implements it via `GtsamTrajectoryOptimizer` (`:72`), translating canonical↔GTSAM at the boundary with no GTSAM types leaking into core (`:8`). The engine dispatches only through the seam (`engine/pipeline/loop_closure_optimize_pipeline.h:15-18`). The COLMAP adapter already has a `bundle_adjustment` capability slot (`colmap_adapter.cpp:37-41`). A `core::ReconstructionOptimizer`-style seam (mirroring `core::TrajectoryOptimizer`) can therefore be added to core (canonical types only) and implemented by an adapter over COLMAP's `bundle_adjuster` CLI (or Ceres someday). This matches the architecture matrix (COLMAP = SfM/BA, GTSAM = global optimization/fusion, Ceres = NLS/BA per the repo's own docs `docs/development/processing-reconstruction-matrix.md`). **Raw-Eigen discipline:** adapters may use raw Eigen; core (seam + canonical types + `core/geometry/` projectors) must not expose raw Eigen tokens (`pose_graph_helpers.h:48,69` pre-existing debt unchanged; new code must add zero).

### Q10 — Is a new `ReconstructionOptimizer` seam needed?
**Yes** — recommended, for the same reasons the `TrajectoryOptimizer` seam exists (D-AB-02: engine never sees a backend). Recommended shape (prose, mirroring `core/trajectory/optimizer.h`):
- Input (canonical): a source `Reconstruction` (points3D + images with `track` + `point2d_idx`), the per-point observation 2D coordinates (resolved from FeatureArtifacts, or carried as an input), the effective camera intrinsics (from `ReconCamera`), an initial guess of image poses, and a `ReconstructionOptimizationOptions` (which parameters to refine: intrinsics yes/no, poses yes/no, points yes/no; max iterations; robust loss; BA-report thresholds).
- Output (canonical): optimized `Reconstruction`-shaped data (optimized poses, refined points3D with updated `error`/`track`, optionally refined intrinsics), plus a `CoreOptimizationTrace`-style telemetry block (RMS reprojection before/after, iterations, converged, inlier/outlier counts) analogous to `OptimizationTrace` (`optimizer.h:38-45`). Optionally reuse the existing `OptimizationResult` (`optimization.h:55`) pattern for lineage/DB persistence.
- Composition: feeds the same consumer chain that `ApplyOptimizedTrajectory` (`core/trajectory/reconstruction_feedback.h:103`) already uses. The full P3-8 chain is: `ApplyOptimizedTrajectory` → new v2 (poses only) → `ReconstructionOptimizer` seam (BA via adapter) → new v3 (points+poses+reprojection) → `AddReconstruction` + `SetReconstructionStatus` supersede. The seam should be **header-only + canonical**, consistent with `reconstruction_feedback.h` and `pose_graph_helpers.h` style (§ of `P3-impl-7-verification-report.md:27`).
- It composes with `ApplyOptimizedTrajectory` in either order (BA-before or BA-after pose write-back); recommend BA operate on the optimized poses produced by the seam so the two stages form a single, auditable revision chain (see Q12).

### Q11 — How is immutable reconstruction lineage preserved?
Confirmed append-only. `AddReconstruction` (`metadata_db.cpp:1830`) issues a plain `INSERT` with a fresh `reconstruction_id` — a new revision is a **freshly-inserted row**, never an UPDATE of the old one. `SetReconstructionStatus` (`metadata_db.h:464`; `metadata_db.cpp:1932`) transitions `succeeded → superseded` only (valid transitions enumerated at `metadata_db.h:458-461`), terminal after supersede. `QueryLatestReconstructionByScene` (`metadata_db.cpp:1863`) returns the newest `status='succeeded'` row by `created_at_ns DESC LIMIT 1`. `FindReconstructionsByScene` (`metadata_db.cpp:1898`) returns all rows in `created_at_ns` order, making the revision chain auditable. Revision **numbering** is not an explicit integer column — lineage is inferred from `created_at_ns` ordering + the `status` supersede chain + provenance `input_artifact_hashes` lineage appended by `ApplyOptimizedTrajectory` (`reconstruction_feedback.h:144-180`). The PK pattern in `0008_trajectory_pose_graph.sql` (`:15,33,51,65,85` all `BLOB PRIMARY KEY` on a UUID identity) is the same style the reconstructions table uses (migration 0007, `metadata_db.cpp:1838-1840`). **Conclusion:** a new revision is append-only by construction; a future revision needs no lineage column, only a new insert + supersede of the prior.

### Q12 — How to refresh points after optimized poses?
Recommended data flow:
1. Read the newest `succeeded` Reconstruction (`QueryLatestReconstructionByScene`, `metadata_db.cpp:1863`) and its cameras/images/points.
2. Apply optimized poses (already produced by the P3-7 `ApplyOptimizedTrajectory` seam, `reconstruction_feedback.h:103`).
3. Resolve per-image 2D keypoints from FeatureArtifacts via `frame_id → FeatureSet → artifact_ref` (`feature_set.h:23`; `feature.schema.json:12-25`), matched to each `TrackElement.point2d_idx` (`reconstruction.h:68`).
4. Feed new poses + intrinsics + observations into re-triangulation. **Three candidate sources for the NEW `ReconPoint3D` list:**
   a. **In-repo triangulation** (recommended for 8b): a new `core/geometry` triangulation primitive (greenfield, Q6) produces xyz from ≥2 observation rays.
   b. **COLMAP re-run** (mapper): re-run the external binary parameterized by `ColmapConfig` (`colmap_config.h`) — but the worker currently has no "fixed poses, re-triangulate only" mode, and it cannot carry the optimized poses (the bin writer for poses is not exposed; only the raw 3-file parse exists).
   c. **Adapter-provided points** (COLMAP `point_triangulator` / `bundle_adjuster` output) inside the 8c seam.
   For a principle-proving milestone, (a) in-repo is the controllable, deterministic choice; (c) is the production/BA path. The worker parameterization precedent (which stages run, determinism pins) is `ColmapConfig::Plan()`/`FromJson` (`colmap_config.h:92-119`) and `colmap_cli.cpp:58-82`.

### Q13 — Which schemas must be extended?
Relevant schemas: `schemas/json/reconstruction.schema.json`, `calibration.schema.json`, `camera` (there is no `camera.schema.json`; camera data lives inside `reconstruction.schema.json` `ReconCamera` definition `:93`), `trajectory.schema.json`, `pose-graph.schema.json`, `loop-closure.schema.json`. **Canonical place for reprojection-error/residual reporting already exists**: `engine/pipeline/quality/quality_report.h:28-57` defines `ReprojectionMetrics{rmse_px, mean_error_px}` and `QualityReport.reprojection` (`:57`), serialized by `quality-report.schema.json` (group `reprojection` with `rmse_px`/`mean_error_px`; RFC-0005 hierarchical, additive). This is the existing QC/quality field pattern (`grep` for `error|rms|inlier|outlier|residual` also finds `ReconPoint3D.error`, `ReprojectionMetrics`, `LoopClosure.geometric_residual` `core/trajectory/loop_closure.h:59`, `OptimizationResult.initial/final_error` `optimization.h:61-63`). **Recommendation:** expose BA/re-triangulation residuals through the existing `QualityReport.reprojection` group (adding an active stage/metric group as needed per the additive RFC-0005 policy `quality-report.schema.json:32`), and use `reconstruction.provenance.backend_specific_json` (`reconstruction.h:103`) for backend-specific BA stats — no new JSON schema required for the metric itself. No existing schema needs hard breaking change; `reconstruction.schema.json` already has `error`/`track` required. (`reconstruction-pipeline.md:96` already specifies reprojection error / accuracy as quality criteria for the sparse stage.)

### Q14 — Which DB migrations are really needed?
The migrations folder contains exactly **7** files: `0001_init.sql`, `0003_scheduler.sql`, `0004_execution_manifest.sql`, `0005_import_rejections.sql`, `0006_sensor_calibration_validity.sql`, `0007_reconstruction.sql`, `0008_trajectory_pose_graph.sql` (verified via glob; note there is **no 0002**). The migration runner confirms 0001,0003..0008 (`metadata_db.cpp:174-280`). Because new reconstructions are append-only inserts (`Q11`) and `QualityReport` already carries the metrics (`Q13`), the **MINIMAL set is zero migrations** for core 8a/8b/8c — revision lineage, supersede, and residual reporting all fit existing schema. Only if an explicit revision-lineage index/column or a dedicated BA-artifact table is desired would **one** migration (0009) be warranted; it is not required for the milestone.

### Q15 — How to prove improvement quantitatively?
Follow P3-7's style (positive E2E proving `error_after < error_before` on a deterministic closed-square fixture).
- **BEFORE baseline:** the existing COLMAP sparse model reconstruction read via `QueryLatestReconstructionByScene`; loop closure; GTSAM optimize via `TrajectoryOptimizer` seam; `ApplyOptimizedTrajectory` produces v2 (poses corrected, points unchanged).
- **AFTER:** re-triangulate + (optionally) BA via the 8c seam producing v3.
- **Metric definitions (prose):**
  - Per-observation residual: `r_ij = sqrt((x_ij − u_ij)^2 + (y_ij − v_ij)^2)` where `(x_ij,y_ij)` is the FeatureArtifact keypoint (`feature.schema.json`) and `(u_ij,v_ij)` is the projected 2D of `ReconPoint3D.xyz` through `ReconImage.pose` + `ReconCamera` intrinsics.
  - `reprojection_RMS = sqrt( (1/N) * Σ r_ij² )` over all inlier observations (N = total observations across all points' tracks).
  - `mean_error_px` = arithmetic mean of residuals (matches `ReprojectionMetrics.mean_error_px`, `quality_report.h:30`).
  - Inlier/outlier rule: a residual is an inlier iff `r_ij ≤ rms_threshold` where `rms_threshold` is a deterministic rule (e.g. `3 × median` or a fixed `k` px), matching the loop-closure verifier's stylistic threshold usage (`pose_graph_helpers.h:200-204`). Outliers are excluded from RMS but counted and reported.
- **Acceptance (minimum bar):** `reprojection_RMS_after < reprojection_RMS_before` (strict, with a margin guideline e.g. ≥ some %), **and** geometry stays valid (`ValidateOptimizedReconstruction`-style structural checks, `reconstruction_feedback.h:230-244`, plus per-point `error` finite), **and** `3D points BEFORE != 3D points AFTER` when poses changed (assert at least one `ReconPoint3D.xyz` differs; the points are not byte-identical to v2).
- **Reusable fixtures:** `tests/unit/test_gtsam_adapter.cpp` — `ClosedSquareDriftedNodes` (`:1705`), `ClosedSquareTruthNodes` (`:1717`), `DriftReductionWithLoop` (`:1783`), `MetricClosedSquareDriftedNodes` (`:2124`), `MetricClosedSquareTruthNodes` (`:2137`). `tests/unit/test_loop_closure_optimize_pipeline.cpp` — `DriftedSquare` (`:70`) and `MakeClosure` (`:81`) build the accepted metric closure with a real non-identity relative pose. `test_loop_closure_to_pose_graph.cpp` `DriftedSquare` (`:79`). A synthetic closed-square reconstruction fixture (points observed by several images with known pixel coordinates and true poses + injected drift) would live in the same directory following the `core/geometry/direction_math.h`-style helper pattern (a small header-only fixture builder), mirroring how `MetricClosedSquareDriftedNodes` is used.
- **Validity-preserved assertions:** unit quaternions (`reconstruction_feedback.h:238-242`), finite `xyz`/`error`, all `track.image_id/point2d_idx` in-range against the images/keypoints, monotonically non-worse `mean_error_px` optional, and appended lineage/provenance intact (`input_artifact_hashes`).

---

## 3. Milestone Definition

### 3.1 Scope (explicit)
- **8a — Reprojection / consistency model + metrics (D2 camera-model constraint):** add the missing projection + back-projection + distortion primitives in `core/geometry/` (raw-Eigen confined); a `ReprojectError`/consistency evaluator in core (canonical, no backend); and wire per-point/per-image reprojection residuals into the existing `QualityReport.reprojection` group (`quality_report.h:28-31`) as a reproducible, additive metrics path. The FIRST normative camera-model set is **`pinhole` plus the distortion models the canonical model already represents** (`core/reconstruction/reconstruction.h:36` list: `pinhole, opencv, opencv_fisheye, fov, omnidirectional, custom`). Every model outside the normative set is either `supported` or **explicitly `unsupported → fail closed`** (see decision D2 and §4 answer B); a model is NEVER silently treated as a different model (e.g. `fisheye`→`pinhole`).
- **8b — Re-triangulation → immutable new revision (D2 primary path):** an **in-repo CANONICAL triangulation** primitive (greenfield, Q6) in `core/geometry/` fed by new poses + calibration + FeatureArtifact observations (Q12); produces a NEW immutable `Reconstruction` revision (append-only insert + `SetReconstructionStatus` supersede, Q11) whose `ReconPoint3D` refresh matches the corrected poses (Q15 assertion: points differ from v2). In-repo canonical triangulation is the PRIMARY path; COLMAP `point_triangulator` is only an alternative/production adapter later and never the only way to obtain canonical points, and COLMAP's native data model must never become the permanent platform model.
- **8c — Bundle adjustment via a seam/adapter boundary (D3 fixed intrinsics):** a new canonical `core::ReconstructionOptimizer` seam (mirroring `core::TrajectoryOptimizer`, Q10) + an adapter implementation that invokes the external COLMAP `bundle_adjuster` (adding it to the `ColmapStage` vocabulary, `colmap_config.h:30-34`, and the CLI mapping `colmap_cli.cpp:29-39`), capturing BA report stats back. For 8c the seam treats `fx fy cx cy distortion` as **FIXED inputs**; BA refines poses and 3D points ONLY (D3). GTSAM remains the global-optimization layer; BA runs through the COLMAP/Ceres-style adapter seam (per the architecture matrix and Q9). Intrinsics refinement is deferred to a separate later increment (8d).

### 3.2 NON-GOALS (do NOT design)
Multi-session mapping; SLAM (ORB-SLAM3/KISS-ICP); LiDAR; GNSS/RTK; Insta360/LingBot; AI backends (VGGT/DUSt3R/MASt3R/Gaussian Splatting); dense reconstruction/OpenMVS; semantic reconstruction.

**Hard architecture-layer-split invariant (do NOT collapse):**
- **GTSAM = global pose/constraint optimization** (the `TrajectoryOptimizer` seam, `core/trajectory/optimizer.h`).
- **COLMAP/Ceres = photogrammetric bundle adjustment** (reached only through a `ReconstructionOptimizer`-style seam/adapter).
- **core/geometry = canonical projection / reprojection / triangulation** (the platform's own math; the single source of truth).
- **Do NOT replace GTSAM by full photogrammetric BA**, and do NOT perform photogrammetric BA inside GTSAM — GTSAM stays the global-optimization layer; BA is reached only through the COLMAP/Ceres-style adapter seam. Likewise do NOT absorb the canonical projection/triangulation into a backend's native model (D2): COLMAP's native data model is an adapter detail, never the permanent platform model.

**D3 fixed-intrinsics scope (do NOT design):** for 8c no intrinsics-DOF refinement (`fx fy cx cy distortion`) — they are fixed inputs; intrinsics refinement is deferred to the separate later increment 8d.

No inline 2D coordinate storage inside the reconstruction document (2D coords stay in FeatureArtifacts; only indexes travel).

### 3.3 Phasing recommendation (D1: separate gated increments, each independently provable)

End-to-end layer stack (each layer provable on its own):
`Reconstruction v1 → GTSAM global pose optimization → ApplyOptimizedTrajectory → v2 (optimized poses, OLD points) → re-triangulation → v3 → Bundle Adjustment → v4`.

- **Increment `P3-impl-8a` — Canonical projection + reprojection geometry (metrics/consistency):** 8a is the dependency for 8b (residual computation needs projection) and 8c (BA needs a residual definition). Deliverable: `core/geometry` projection/back-projection/distortion primitives + reprojection evaluator + `QualityReport` residual wiring + unit tests, per the D2 normative camera-model set. Independent of any backend; testable with a synthetic closed-square fixture. Proves 8a alone.
- **Increment `P3-impl-8b` — Canonical re-triangulation:** depends on 8a projection + observation resolution (Q12). Deliverable: in-repo canonical triangulation primitive (D2 primary path) + revision-creation path (new immutable revision v3 + supersede), producing the "points refreshed vs. v2" assertion (Q15). Proves 8b alone.
- **Increment `P3-impl-8c` — Bundle Adjustment (fixed intrinsics):** depends on 8a metric model and 8b revision plumbing. Deliverable: the `ReconstructionOptimizer` seam + COLMAP `bundle_adjuster` adapter invocation + report capture over the v3 points (D3: fixed intrinsics, refine poses + points only); GTSAM untouched. Proves 8c alone; produces v4.
- **Increment `P3-impl-8d` (optional, OUT OF SCOPE for 8a–8c):** intrinsics refinement (fx/fy/cx/cy/distortion DOF in BA). A separate later increment; not planned here.
- Each increment is independently gated and landed with its own test plan mirroring Q15 (in the D1 order above).

### 3.4 Invariants / Gates
- No raw-Eigen growth in core: `check_domain_types.py` violation count must remain 2 (pre-existing `pose_graph_helpers.h:48,69`); new projection/triangulation/residual code in `core/geometry/` and adapters only.
- Immutability: no revision ever mutates a prior row; revisions are append-only inserts + terminal supersede (`metadata_db.h:458-461`).
- **Architecture-layer split (hard):** GTSAM = global pose/constraint optimization (`optimizer.h`); COLMAP/Ceres = photogrammetric BA (seam/adapter only); core/geometry = canonical projection/reprojection/triangulation (single source of truth). No collapsing of any layer onto another (§3.2).
- **No silent model substitution (D2):** a camera model that is not in the normative set is `supported` or `unsupported → fail closed`; a model is never silently treated as another model (e.g. `fisheye`→`pinhole`).
- (D1) Each layer (2D projection, re-triangulation, BA) is independently provable and gated as its own increment 8a/8b/8c.
- Seam discipline: `engine/` never sees COLMAP/GTSAM/Ceres types; BA dispatches through `core::ReconstructionOptimizer` only (`loop_closure_optimize_pipeline.h:15-18` pattern).
- Schema stability: `check_schemas.py` green with **zero** migrations for 8a–8c (D4 concludes the minimal set is ZERO; `ReconPoint3D.error`/`track` required fields preserved, `reconstruction.schema.json:194`).
- Determinism: identical inputs → identical output bytes (ADR-020), consistent with `ReconstructionToJson` determinism (`colmap_converter.cpp:297-303,602`); for BA the determinism bar applies to derived metrics/lineage (D6).

### 3.5 Test plan (prose)
- Unit: `core/geometry` projection/back-projection round-trip and distortion; triangulation on ≥2 rays (cheirality in front of both cameras per `P3-impl-7c-implementation-readiness.md:263`) and degenerate-reject; residual evaluator against known-good pixel coords.
- Integration: extend `test_loop_closure_optimize_pipeline.cpp` (reuse `DriftedSquare` `:70`) and `test_gtsam_adapter.cpp` (`MetricClosedSquareDriftedNodes` `:2124`) into a full 8a→8b→8c chain producing v3; assert `reprojection_RMS_after < before`, geometry valid, points differ from v2, lineage intact.
- DB: revise-append + supersede + `QueryLatestReconstructionByScene` repoint (reuse `test_reconstruction_feedback.cpp` revision patterns, e.g. `OptionAPointsUnchanged` `:67` and `RevisionSemantics`).
- Full Debug+Release `ctest` plus all `check_*.py` gates, mirroring the P3-impl-7 verification-report gate list (§8).

## 4. Decisions & resolutions

### 4.1 D1 — BA AFTER `ApplyOptimizedTrajectory` — **APPROVED**

Sequence (each layer independently provable): `Reconstruction v1 → GTSAM global pose optimization → ApplyOptimizedTrajectory → Reconstruction v2 (optimized poses, OLD points) → re-triangulation → Reconstruction v3 → Bundle Adjustment → Reconstruction v4`. 8a = projection/reprojection geometry alone; 8b = re-triangulation alone; 8c = BA alone. §3.3 is updated to four separate gated increments: `P3-impl-8a Canonical projection + reprojection geometry` → `P3-impl-8b Canonical re-triangulation` → `P3-impl-8c Bundle Adjustment (fixed intrinsics)` → `P3-impl-8d (optional) intrinsics refinement — OUT OF SCOPE for 8a–8c, separate later increment`. This closes former open question 1 (BA-after, not BA-first).

### 4.2 D2 — In-repo CANONICAL triangulation is the PRIMARY path — **APPROVED**

`core/geometry → canonical triangulation → ReconPoint` is the primary path. COLMAP `point_triangulator` remains only an alternative/production adapter later and must NEVER be the only way to get canonical points; COLMAP's native data model must NOT become the permanent platform model. **Camera-model scope for 8a:** the FIRST normative set is `pinhole` plus the distortion models the canonical model already represents (`core/reconstruction/reconstruction.h:36`). Everything else is either `supported` or `explicitly unsupported → fail closed`; a model is NEVER silently treated as another model (e.g. `fisheye`→`pinhole`). This closes former open question 2.

### 4.3 D3 — FIXED intrinsics for P3-impl-8 — **APPROVED**

`fx fy cx cy distortion` are FIXED. BA refines poses and 3D points ONLY. Rationale: proving reconstruction consistency after global pose optimization must not be confounded with intrinsics DOF. `ReconstructionOptimizer` input/output treats intrinsics as fixed inputs (not refinement parameters) for 8c. Intrinsics refinement is deferred to 8d (separate later increment). This closes former open question 3; §3.1/§3.2/§3.3 are updated accordingly.

### 4.4 D4 — Residual / reprojection-error storage — **PROPOSED-CLOSED (pending 8a GO)**

- **Decision:** persist per-image/per-point reprojection residuals as a new additive metric group in the existing **`QualityReport`** (`engine/pipeline/quality/quality_report.h:28-31` `ReprojectionMetrics`; `quality-report.schema.json:30-37` open `metrics` object under the RFC-0005 additive Schema Evolution Policy, `quality-report.schema.json:5`). Extend the existing `reprojection` group with additive keys (`rms_px`, `mean_error_px`, per-image aggregates, per-point distribution). Do **NOT** use `provenance.backend_specific_json` (`core/reconstruction/reconstruction.h:103`) as the canonical metric carrier — it stays the site for opaque backend diagnostics. **No `0009` migration is warranted.**
- **Rationale:** `QualityReport` is the ratified, immutable, content-addressed QC artifact (RFC-0005) and already carries a `reprojection` group (`quality_report.h:57`), so the residual channel exists today. `backend_specific_json` is an opaque string blob for provenance; burying the Q15 acceptance gate in it would make the gate un-evaluable. Residuals are part of the report artifact payload (CAS), not DB rows, so no migration is implied (Q14 conclusion: minimal set = ZERO).
- **Affected files:** `engine/pipeline/quality/quality_report.{h,cpp}` (extend `ReprojectionMetrics` `:28-31`; keep `QualityReportFromJson`/`QualityReportToJson` round-trippable `:69-78`, and `AggregateScore` `quality_report.cpp:111` accepting the new group), `schemas/json/quality-report.schema.json:30-37` (documented additive keys, no breaking change), plus the new `core/geometry` reprojection evaluator output consumed by the report. No migration, no `Reconstruction`/`ReconstructionRow` change.
- **Canonical data flow (prose):** 8a computes per-observation residual `r_ij` from the FeatureArtifact keypoint (`schemas/json/feature.schema.json:12-25`) vs. the projected `ReconPoint3D.xyz` through `ReconImage.pose` + `ReconCamera` (projection in `core/geometry`). 8b/8c recompute residuals after re-triangulation/BA. The evaluator aggregates per-point/per-image and emits the deterministic `reprojection` group into a new `QualityReport` artifact (CAS, type `quality_report`, linked via the run manifest per `quality_report.h:7`). The reconstruction document is unchanged (`ReconPoint3D.error` scalar remains per `reconstruction.h:65`).
- **Rejection / fail-closed behavior:** if any per-point aggregate is non-finite or a `point2d_idx`/`image_id` reference is out of range of the FeatureArtifact keypoints, the evaluator returns a typed error and produces NO report and NO revision (finite-check precedent `core/loop_closure/correspondence_reconstruction.h:66-69`). No partial reports.
- **Tests:** extend `tests/unit/test_quality_report.cpp` (determinism `:25-26,47`; parse round-trip `:100-102`) for the additive group; a residual-evaluator unit test against known-good pixels; assert `QualityReportFromJson(QualityReportToJson(r))` round-trips with the new keys; `check_schemas.py` green.
- **Impact on schemas/DB:** none required — `metrics` is open (`quality-report.schema.json:33`). **Migration count: ZERO.**
- **Phase:** 8a (group + evaluator); recomputed by 8b and 8c.

### 4.5 D5 — Outlier / inlier rule and acceptance margin — **PROPOSED-CLOSED (pending 8a GO)**

- **Decision:** inlier/outlier rule = **`3 × median absolute residual (MAD)` with a documented absolute floor of `2 px`**: `r_ij` is an inlier iff `r_ij ≤ max(3 × median(|r|), 2.0 px)`. Outliers are excluded from the RMS but counted and reported. Acceptance gate = **`reprojection_RMS_after < reprojection_RMS_before` with a minimum margin of 10%** (`RMS_after < 0.9 × RMS_before`), a frozen, documented default (per-run configurable above that floor). Deterministic pure functions of run inputs, matching the loop-closure verifier's threshold style (`core/trajectory/pose_graph_helpers.h:198-206` candidate floor, `:276-279` spatial-tolerance guard, `:292-294` inlier-ratio gate).
- **Rationale:** MAD is robust to the heavy-tailed residual distribution of SfM (a few gross outliers inflate RMS but not the median); the 2 px floor prevents pathological mass-rejection on an already-clean model. The 10% reprojection margin is tighter than P3-7's 30% pose-RMSE margin because the reprojection criterion measures inlier consistency directly; re-triangulation under corrected poses should be strictly better, so the gate is a validity guard rather than a stretch target.
- **Affected files:** the 8a residual evaluator (new, in `core/geometry/` + canonical aggregation), the engine orchestration site where the gate is enforced (design-time, mirroring `engine/pipeline/loop_closure_optimize_pipeline.h:58-89` style or a new stage), and the Q15 acceptance tests. No schema change.
- **Canonical data flow (prose):** residuals → median + MAD → per-observation inlier/outlier classification → per-point `error` as the inlier mean (`reconstruction.h:65`) → inlier-only RMS/mean → gate comparison against the v2 BEFORE baseline.
- **Rejection / fail-closed behavior:** if `RMS_after ≥ 0.9 × RMS_before`, the gate FAILS and NO v3/v4 revision is emitted and NO supersede is performed (a worse reconstruction is never persisted). The run reports `FAILED` with the two RMS values and the outlier count.
- **Tests:** synthetic square where v2 (wrong points) has known RMS `B` and v3 after re-triangulation is strictly lower → assert `RMS_after < 0.9×B`; a fixture with 2 seeded gross outliers → assert expected inlier/outlier split matches hand-computed counts; a constructed "no-improvement" case asserts the gate fails and no revision is produced.
- **Impact on schemas/DB:** none. **Migration count: ZERO.**
- **Phase:** rule defined in 8a; enforced in the 8b and 8c gates.

### 4.6 D6 — Determinism scope for BA — **PROPOSED-CLOSED (pending 8a GO)**

- **Decision:** the ADR-020 determinism bar applies to the **derived metrics and lineage** (residual reports, CAS hashes, revision documents, provenance `input_artifact_hashes`) — NOT to the raw COLMAP `bundle_adjuster` output bytes across heterogeneous platforms. Concurrently **pin `ColmapConfig.seed`** (`adapters/colmap/colmap_config.h:82`) for every BA run as best-effort backend reproducibility. The canonical document keeps deterministic ordering/formatting/sorting of whatever the backend emits (`colmap_converter.cpp:297-303` sorts cameras/images/points by id) so each run's CAS payload is well-formed, but bit-identity of v4 across platforms is NOT a gate.
- **Rationale:** COLMAP's numeric solvers are platform/thread-sensitive; requiring bit-deterministic BA output is un-testable and would reject a production tool on infrastructure grounds. What the platform guarantees — and what every gate relies on — is reproducible metrics/reports plus a complete, non-mutating lineage chain. The seed field already exists for this purpose (`colmap_config.h:81-82`).
- **Affected files:** `adapters/colmap/colmap_config.h:81-82` (seed plumbed for the BA stage), the new `core::ReconstructionOptimizer` seam + COLMAP BA adapter, and the deterministic metrics/report generation.
- **Canonical data flow (prose):** BA adapter builds argv via `BuildStageCommand` (`adapters/colmap/colmap_cli.cpp:58-82`) including `--random_seed <seed>`; reads back `points3D.bin`; parses via `ParseSparseModel` (`colmap_converter.cpp:289`); translates deterministically (`SparseModelToReconstruction`, `colmap_converter.cpp:504`); the metrics evaluator recomputes the `reprojection` group from the v4 points; provenance records the v3 CAS hash as input and the seed in `backend_specific_json` (`reconstruction.h:103`).
- **Rejection / fail-closed behavior:** a plan that enables the BA stage without a pinned `seed` is rejected at config validation (`ColmapConfig::FromJson`, `colmap_config.h:96-106` precedent); a missing/incomplete `points3D.bin` after `bundle_adjuster` fails closed via `DiscoverNativeModelFiles` (`colmap_cli.cpp:89-116`); non-finite residuals fail the report step (D4).
- **Tests:** run BA twice with the same seed on the same workspace → assert the DERIVED RMS/mean and lineage hashes are identical (report-level determinism) and ordering sorted by id; NO assertion of cross-platform byte identity of the raw output.
- **Impact on schemas/DB:** none (seed is adapter configuration, not schema). **Migration count: ZERO.**
- **Phase:** 8c (seed also available to an 8b COLMAP re-run if that alternative path is ever used).

### 4.7 D7 — Fixture reuse — **PROPOSED-CLOSED (pending 8a GO)**

- **Decision:** a **shared, header-only synthetic closed-square reconstruction fixture** in the P3-7 pattern, canonical home at **`tests/unit/fixtures/closed_square_reconstruction.h`** (new test-support header). Not duplicated per test.
- **Rationale:** `MetricClosedSquareDriftedNodes` (`tests/unit/test_gtsam_adapter.cpp:2124`) and `DriftedSquare` (`tests/unit/test_loop_closure_optimize_pipeline.cpp:70`) are pose-only, per-file helpers; the reconstruction fixture must add points/observations IDENTICALLY across the 8a/8b/8c tests, or the acceptance gate becomes inconsistent. A shared builder mirrors the `core/geometry/direction_math.h`-style helper pattern; being header-only it needs no `tests/CMakeLists.txt` change.
- **Affected files:** new `tests/unit/fixtures/closed_square_reconstruction.h`; the 8a evaluator test, 8b re-triangulation test, and 8c BA test all include it. Existing pose-only helpers (`test_gtsam_adapter.cpp:1705,1717,2124`) are left untouched (additive only).
- **Canonical data flow (prose):** the fixture builds a small closed-square scene — 5 frames at drifted/truth poses, one shared square of corner 3D points, per-frame FeatureArtifact-shaped keypoints (pixels obtained by projecting the TRUE points through TRUE intrinsics), and a v1-shaped `Reconstruction`. Tests instantiate it, perturb/verify, and feed the pipeline.
- **Rejection / fail-closed behavior:** the builder returns typed values or throws (fail-closed) when constructed from non-finite inputs or conflicting pose/point sets; tests that misuse it fail at construction, never silently.
- **Tests:** a fixture self-check that its projected keypoints reproduce the intended pixels (round-trip); then all 8a/8b/8c tests reuse it.
- **Impact on schemas/DB:** none (test-only header). **Migration count: ZERO.**
- **Phase:** 8a (fixture first, for evaluator tests); reused by 8b and 8c.

### 4.8 A — Coordinate convention — single source of truth (anti double-inversion)

The platform convention is **`T_reconstruction_camera` (camera→reconstruction; world-from-camera)**, exactly as `ReconImage.pose` (`core/reconstruction/reconstruction.h:49,54`), `ReconPose` (scalar-last `rotation_xyzw`, `reconstruction.h:24`), `MakeCameraPose` (`core/trajectory/pose_graph_helpers.h:44-51`), `SE3` (`core/geometry/se3.h:13`) and the typed `WorldFromCamera`/`CameraFromWorld` (`core/geometry/camera_transform.h:15-45`) already encode. Double-letter `T_A B` = maps frame B → frame A (used this way in `reconstruction_feedback.h:214` `T_rc = T_rt * T_tc`).

| Quantity | Name | Definition | Existing anchor |
|----------|------|------------|-----------------|
| world-from-camera, image i | `T_rc_i` | maps camera frame → reconstruction frame | `ReconImage.pose` `reconstruction.h:49,54`; `MakeCameraPose` `pose_graph_helpers.h:44-51` |
| camera-from-reconstruction | `T_cr_i` | `= T_rc_i.Inverse()` | `SE3::Inverse` `se3.h:40-44`; `CameraFromWorld` `camera_transform.h:31-45` |
| 3D point (reconstruction frame) | `p_R` | `ReconPoint3D.xyz` | `reconstruction.h:63` |
| point in camera frame | `p_C` | `= T_cr_i · p_R` (`p_C = Rᵀ(p_R − t)`) | `SE3::TransformPoint` `se3.h:46-48` |
| projection | `u_i = proj_i(p_R)` | `u = K_dist(p_C)`, requires `w_C > 0` (cheirality) | NEW in `core/geometry` (8a) |
| back-projection | `dir_i = unproj_i(u)` | unit ray in camera frame (depth 1) | NEW (8a) |
| relative pose source←target | `T_ij = T_rc_i⁻¹ · T_rc_j` | odometry/edge relative transform | `RelativePoseBetween` `pose_graph_helpers.h:64-73` |

Inverse relationships / invariants (must hold, enforced by tests):
- `T_rc · T_cr = Identity`; `SE3::Inverse` round-trip.
- `unproj(proj(p_R)) == dir` (unit ray) and `proj(unproj(u)) == u` within the 8a tolerance (D2/answer B).
- `p_C = T_cr · (T_rc · p_C)` identity round-trip for all finite `p_C`.
- relative pose is never "world-from-body vs body-from-world" swapped: `T_ij` follows `RelativePoseBetween` semantics exactly.
- Quaternions unit-norm after every rotation path (`reconstruction_feedback.h:238-242` precedent).

**Tests:** cross-frame invariant — the same 3D point observed by two cameras with true poses projects to the fixture's expected pixels (fixture self-check); inverse round-trips above; unit-quaternion checks; a negative test that an inversion swap (`T_rc` misused as `T_cr`) is caught by the round-trip tolerances.

### 4.9 B — Distortion: normative set, round-trip, fail-closed

First normative set (D2) = `pinhole` plus the distortion models already in the canonical model (`core/reconstruction/reconstruction.h:36` intrinsic_model enum; `reconstruction.h:41` distortion_model enum; reconciled with `schemas/json/calibration.schema.json:25`).

| intrinsic_model | distortion_model | 8a support | project / unproject |
|-----------------|------------------|-----------|---------------------|
| `pinhole` | `none` | first-class | closed-form, exact `K(p_C)`, identity distortion; `unproj` = `K⁻¹` |
| `opencv` | `opencv_radial` | first-class | radial-tangential `[k1,k2,p1,p2,(k3)]` (normative OpenCV/COLMAP parameter order, 4 or 5 coefficients); `unproj` via fixed-point distortion inversion (documented convergence tolerance) |
| `opencv_fisheye` | `opencv_fisheye` | first-class | equidistant / Kannala–Brandt formulation (COLMAP OPENCV_FISHEYE, `colmap_converter.cpp:51-63`); `unproj` via iterative or closed-form inverse |
| `fov` | `none`/`custom` | first-class | FOV-division with `omega` (COLMAP FOV, `colmap_converter.cpp:59`); closed-form `project`/`unproject` |
| `omnidirectional` | — | **unsupported in 8a → fail closed** | until a parameterization is defined in a later increment; never treated as `pinhole` |
| `custom` | `custom` | **unsupported in 8a → fail closed** | COLMAP FULL_OPENCV / THIN_PRISM_FISHEYE (`colmap_converter.cpp:58,62`); never treated as `pinhole` |
| `opengl` / anything else (`calibration.schema.json:25` surplus) | any | **unsupported in 8a → fail closed** | not in the canonical set (`reconstruction.h:36`); rejected at model-selection |

Round-trip tests (8a), for EVERY first-class model: `3D → project → 2D → unproject → ray → recover 3D → project → 2D`. **Expected tolerance:** pixel round-trip error `< 1e-5 px` for all first-class models (pinhole is exact to floating point, `< 1e-6 px`); the recovered ray is a UNIT ray (`‖dir‖ = 1` within `1e-9`); `proj(unproj(u))` reproduces `u` within the same tolerance; distortion-free sub-models act as identity distortion. Each iterative model carries a documented fixed-point convergence criterion.

Fail-closed: a `ReconCamera` whose `intrinsic_model`/`distortion_model` combination is outside the first-class set triggers a typed validation error at the model-selection point (projection evaluator / seam input validation) — `ValidationError` precedent `adapters/colmap/colmap_config.h:96-106` — BEFORE any computation. A negative test asserts e.g. `opencv_fisheye → pinhole` substitution is impossible.

### 4.10 C — Triangulation acceptance predicate (fail-closed point emission)

A NEW `ReconPoint` is emitted ONLY if ALL of:
1. **Positive depth in BOTH cameras:** for every observing camera, the back-projected ray to the candidate must have positive camera-frame depth `w_C > 0` (cheirality — the solution must place points in front of both cameras, per `docs/architecture/P3-impl-7c-implementation-readiness.md:263`).
2. **Finite XYZ:** all three coordinates finite.
3. **Reasonable parallax:** the minimum angle between the observation rays ≥ `θ_min` (default `2°`, configurable, deterministic). Near-parallel / zero-baseline configurations are rejected (mirrors the D1/zero-baseline guard `pose_graph_helpers.h:420`).
4. **Reprojection residual below threshold:** the candidate's inlier residuals satisfy the D5 deterministic rule (`r_ij ≤ max(3×median, 2 px)`), so a point is never emitted on the back of a bad observation.

If **ANY** check fails → **NO `ReconPoint` is emitted** (fail-closed): the observation set is classified REJECTED (§4.11), no candidate is inserted, no `point3d_id` is consumed, and no best-effort point is ever written. Grounds the D2 "canonical triangulation → ReconPoint" primary path; the 8b test matrix covers: happy path (front of both), behind-one-camera rejected, behind-both rejected, parallel rays (zero parallax) rejected, high-residual rejected, non-finite/NaN XYZ rejected.

### 4.11 D — Point identity / lineage (old vs. new `ReconPoint.id`)

Classification applied per v2 point when producing v3 (and extended to v4 by BA):
- **PRESERVED** — the point's observations survived and recomputed geometry moved by `< τ` (stability threshold, e.g. `0.1×` mean camera baseline, configurable): carried forward with its ORIGINAL `point3d_id`, content byte-stable.
- **REPLACED** — observations matched but geometry moved `≥ τ`: the old point is dropped from v3 as-is and re-emitted as a NEW point with a NEW `point3d_id` (tie via the observation track, `reconstruction.h:66-71`).
- **NEW** — triangulated from observations with no v2 counterpart: NEW `point3d_id`.
- **REJECTED** — observations failing the acceptance predicate (§4.10): no point, no ID consumed.

**ID scheme:** new points always receive a NEW reconstruction-scoped `point3d_id` (`reconstruction.h:62`) — never reuse an old ID for recomputed geometry, never mutate a v2 point in place (immutability `metadata_db.h:458-461`). Correspondence across revisions is established by the observation track, not by ID.

**Provenance record:** `ReconstructionProvenance.backend_specific_json` (`reconstruction.h:103`) records the classification summary (PRESERVED/REPLACED/NEW/REJECTED counts + the v2 CAS hash link); the `input_artifact_hashes` chain inherits and appends the v2 CAS hash plus re-triangulation inputs, exactly mirroring `ApplyOptimizedTrajectory` (`core/trajectory/reconstruction_feedback.h:144-180`: fresh `backend.name="spatial_retriangulator"` provenance, hashes sorted ascending, no self-reference).

**Tests:** a fixture seeded with one known bad point → assert class counts; a REPLACED point has a different ID than its old counterpart; PRESERVED points byte-equal; lineage hashes chain v2→v3→v4 acyclically.

### 4.12 E — Reconstruction revision semantics (append-only chain)

Append-only chain: `v2 → (re-triangulation) → v3 → (BA) → v4`, never in-place mutation.

- **Stages that CREATE a revision (fresh `INSERT` via `MetadataDb::AddReconstruction`, `core/storage/metadata_db.cpp:1830`; `metadata_db.h:450`):**
  - `ApplyOptimizedTrajectory` produces v2 from v1 (`core/trajectory/reconstruction_feedback.h:103`, new `reconstruction_id` per `:137`).
  - Re-triangulation stage (8b) produces v3 from v2.
  - `ReconstructionOptimizer`/BA stage (8c) produces v4 from v3.
- **Stages that ONLY UPDATE status (never content):** the supersede step `SetReconstructionStatus(older_id, "superseded")` (`metadata_db.h:464`; valid transitions `:458-461`: `succeeded → superseded`, terminal after; a new row is always `succeeded`). `QueryLatestReconstructionByScene` repoints implicitly by picking the newest `status='succeeded'` row (`metadata_db.cpp:1863-1895`).
- Invariant: exactly one `succeeded` revision per scene after each stage completes (mirrors `RevisionSemanticsSucceedsInSameScene`, `tests/unit/test_reconstruction_feedback.cpp`); a stage that fails its D5 gate emits no new row and no supersede.

| Stage | Creates | Updates | Result |
|-------|---------|---------|--------|
| GTSAM pose optimization (P3-7) | — (persists `OptimizationResultRow` via `UpsertOptimizationResult`, `loop_closure_optimize_pipeline.h:10-12`) | — | input to v2 |
| `ApplyOptimizedTrajectory` | v2 row | — | v2 `succeeded` (v1 later `superseded`) |
| Re-triangulation (8b) | v3 row | v2 → `superseded` | v3 `succeeded` |
| BA (8c) | v4 row | v3 → `superseded` | v4 `succeeded` |

Every revision has a documented provenance reason; no revision is silently skipped or re-numbered; `FindReconstructionsByScene` (`metadata_db.cpp:1898`) keeps the full auditable chain.

---

## Addendum 8a — IMPLEMENTED (2026-09-05)

This addendum records what P3-impl-8a actually did. It supersedes the "readiness only / ZERO code changes" claim above for increment 8a and covers the normative §4.8–4.10/4.12 guards that 8a was responsible for. 8b (re-triangulation → v3), 8c (BA, fixed intrinsics → v4) and 8d (intrinsics refinement) are **NOT started**.

### What 8a delivered

| Area | Deliverable | Path |
|------|-------------|------|
| First-class camera models (§4.9) | `CameraModel` (`kPinhole`, `kOpenCvRadial`, `kOpenCvFisheye`, `kFov`) with `Project` (cheirality `w_C > 0`), `Unproject` (unit ray), `DistortUnit`/`UndistortUnit`, `FromReconCamera` fail-closed model selection | `core/geometry/camera_model.h` (new, header-only) |
| Deterministic residual evaluator (§4.8, D5) | `InitializeReprojectionViews` (ordered by `image_id`, `T_rc` pose per §4.8, fail-closed per camera), `ProjectPoint`, `ComputeObservationResidual`, `EvaluateReprojection` (inlier-only RMS, median, threshold, per-image/per-point aggregates, deterministic ordering), `ReprojectionThreshold` = `max(3·median, 2.0 px)` | `core/geometry/reprojection.h` (new, header-only) |
| Reprojection gate (D5) | `PassesReprojectionGate(rms_after, rms_before, factor=0.9)` — strict `<`, false for non-finite/negative; reusable pure predicate, no writes, no revision | `core/geometry/reprojection.h` |
| QualityReport integration (D4) | `engine::ReprojectionMetrics` is now `using = spatial::core::geometry::ReprojectionMetrics` (single source of truth); `QualityReportToJson`/`QualityReportFromJson` round-trip the additive keys `median_error_px`, `threshold_px`, `inlier_count`, `outlier_count`, `total_count`, `per_image[]`, `per_point[]` under the RFC-0005 additive policy | `engine/pipeline/quality/quality_report.{h,cpp}` (extended) |
| Shared fixture (D7) | Header-only closed-square reconstruction fixture: 5 frames (truth + drifted poses), 4 square corners, pinhole `fx=fy=500, cx=320, cy=240` (640×480), keypoints = truth-projected pixels, v1-shaped `Reconstruction` with drifted poses, `ReconPoint3D.error` under drifted geometry; fail-closed construction; `SelfCheck` | `tests/unit/fixtures/closed_square_reconstruction.h` (new, header-only) |
| Tests | 34 GTests in `spatial_reprojection_geometry_tests` covering: §4.8 inverse round-trip + anti-double-inversion + T_rc-as-T_cr caught by tolerance; §4.9 pixel round-trip `<1e-5 px` (pinhole `<1e-6`), unit-ray `‖dir‖=1` within `1e-9`, distortion/un-distortion round-trip for all four first-class models; fail-closed (`ValidationError`) for degenerate intrinsics, non-finite coefficients, wrong coefficient counts, unsupported/`omnidirectional`/`custom` models, and the explicit `opencv_fisheye → pinhole` substitution negative (a fisheye selects `kOpenCvFisheye`, never pinhole; a fisheye distortion declared on a pinhole intrinsic fails closed at selection); deterministic evaluator metrics; D5 hand-computed inlier/outlier split and threshold; evaluator fail-closed on unknown image/point refs and non-finite keypoints; gate predicate incl. strict-`<` boundary and non-finite rejection; fixture self-check + fail-closed construction; `QualityReportFromJson(QualityReportToJson(r))` round-trip with the new keys | `tests/unit/test_reprojection_geometry.cpp` (new) |
| Build registration | New target `spatial_reprojection_geometry_tests` (core + engine + fixture, GTest) | `tests/CMakeLists.txt` |

### Conventions locked in

- `opencv_radial` coefficient storage order is OpenCV/COLMAP **`[k1, k2, p1, p2, (k3)]`** (4 or 5 coefficients accepted) — note this differs from the tabular "(k1,k2[,k3],p1,p2)" reading of §4.9; the OpenCV/COLMAP order is canonical.
- `opencv_fisheye` = COLMAP `OPENCV_FISHEYE` `[k1,k2,k3,k4]`; `fov` = `[omega]` closed-form (Devernay–Faugeras).
- `FromReconCamera` fail-closed mapping: `pinhole` requires distortion `none`/empty; `opencv` requires `opencv_radial`; `opencv_fisheye` requires `opencv_fisheye`; `fov` accepts any distortion field (advisory). Anything else throws `ValidationError(kValidationDomain)` BEFORE computation — never approximated as another model.
- Poses are `ReconImage.pose.rotation_xyzw` (scalar-last) normalized + `translation_xyz`, wrapped in typed `WorldFromCamera`/`CameraFromWorld` (`camera_transform.h`); residual `r_ij = ‖keypoint − proj_i(p_R)‖`; RMS/mean computed over inliers only; outliers counted and reported, never dropped silently.
- Raw Eigen remains confined to `core/geometry/` + `adapters/` (gate `check_domain_types.py` stayed at the 2 pre-existing `core/trajectory/pose_graph_helpers.h` violations — nothing new).
- The report group is additive only (RFC-0005); zero migrations, zero `Reconstruction`/schema/DB changes, no revision rows, no gate writes. The gate is a pure predicate for 8b/8c to enforce.

### Gates / verification

- Build & tests: Debug and Release both **636/636** pass (34 new 8a tests + full suite).
- `check_domain_types.py`: 2 pre-existing violations (unchanged). `check_dependencies.py`, `check_schemas.py`, `check_worker_boundary.py`, `check_arch_debt.py`, `check_rfc.py`: PASS.
- `check_constitution.py --base HEAD --rfc RFC-0002`: PASS (4 protected paths: the two new `core/geometry/` headers + the two extended `engine/pipeline/quality/` files; change-control via RFC-0002, the permanent-spatial-data-model/geometry governing RFC).

### 8b / 8c / 8d (NOT started)

Re-triangulation acceptance predicate (§4.10), point identity/lineage (§4.11), revision append-only chain + supersede (§4.12), the in-image FeatureArtifact keypoint resolver, gate enforcement on v3/v4, BA seed plumbing (D6), and intrinsics refinement (D7/8d) remain future increments on top of this base.

---

## Addendum 8b — IMPLEMENTED (2026-09-06)

This addendum records what P3-impl-8b actually did: the canonical in-repo re-triangulation algorithm that consumes a v2 `Reconstruction` (optimized poses from `ApplyOptimizedTrajectory`) and produces an immutable v3 revision with refreshed 3D points and full lineage classification. The text above ("8b/8c/8d NOT started") is superseded for 8b only; 8c (BA, fixed intrinsics → v4) and 8d (intrinsics refinement) are **NOT started**.

### What 8b delivered

| Area | Deliverable | Path |
|------|-------------|------|
| Canonical re-triangulation (§4.2, D2) | `Retriangulate(v2, observations, options)` — pure, header-only, no DB writes (§4.12 seam boundary). DLT closest-point of the first two track rays combined with the §4.10 acceptance predicate and global D5 gate; produces v3 with fresh identity | `core/geometry/triangulation.h` (new) |
| Two-ray primitive | `TriangulateTwoRays(view1, pixel1, view2, pixel2, min_parallax_deg)` → `TriangulationCandidate{xyz, parallax_deg, acceptance}`; unprojects pixels to unit rays, transforms to world frame (`WorldFromCamera.rotation` per §4.8), intersects via DLT midpoint (corrected signs: `t=(C·D−B·E)/denom`, `s=(B·D−A·E)/denom`) | `core/geometry/triangulation.h` |
| Acceptance predicate (§4.10) | `TriangulationAcceptance` enum: `kAccepted` / `kRejectedNonFinite` / `kRejectedNegativeDepth` / `kRejectedLowParallax` / `kRejectedHighResidual`; checks destinct image_ids, finite XYZ, positive depth in BOTH cameras, parallax ≥ `min_parallax_deg` (default 2°), D5 reprojection gate (mean residual vs global threshold) | `core/geometry/triangulation.h` |
| Reprojection residual (D5) | `ComputePointReprojectionError` — mean per-pixel residual `‖keypoint − proj(p_R)‖` across ALL track views (not just the triangulation pair); rejects behind-camera (`z≤0`) and non-finite projections as non-acceptable | `core/geometry/triangulation.h` |
| Lineage (§4.11) | `PointLineage` enum (`kPreserved`, `kReplaced`, `kNew`, `kRejected`); per-point `PointLineageEntry` + `RetriangulationLineageSummary` (counts + per-point entries); REPLACED/NEW points always get a NEW `point3d_id` (never reuse), PRESERVED keeps the original id, correspondence is via track/observations not id | `core/geometry/triangulation.h` |
| Revision creation (§4.12) | v3 baseline copy of v2 (cameras/images/metadata), fresh `reconstruction_id` (`FormatUuid(GenerateUuid())`), `status="succeeded"`, deterministic `created_at_ns=0`, provenance `backend.name="spatial_retriangulator"` v1.0.0, ascending sorted `input_artifact_hashes` = inherited + v2 id (no self-reference); no DB write, no supersede (that is the adapter's job in a later phase) | `core/geometry/triangulation.h` |
| Fail-closed | `ValidationError(kValidationDomain)` for non-finite keypoints in observations; unknown `image_id` / missing observation reference → `kRejectedNonFinite` (no point emitted); unsupported camera model fails closed at view construction (reuses `camera_model.h` `FromReconCamera`) | `core/geometry/triangulation.h` + `core/geometry/camera_model.h` |
| Tests | 15 GTests in `spatial_triangulation_tests`: DLT ground-truth recovery `≤1e-6 m` (negligible-noise, exact pixels), multi-point triangle accuracy, parallax zero/near-zero/behind-camera rejects, NaN/Inf keypoints throw, truth-poses → ground-truth points, drifted poses → REPLACED points differing from truth, 50px-outlier point rejected via D5 while clean median keeps threshold at 2px floor, fresh revision (new id, no self-reference), unsupported-model fail-closed, missing-observation fail-closed, and the opencv_radial coefficient-order interop pin `[k1,k2,p1,p2,(k3)]` | `tests/unit/test_triangulation.cpp` (new) |
| Build registration | New target `spatial_triangulation_tests` (core + engine + fixture, GTest) | `tests/CMakeLists.txt` |

### Conventions locked in

- Coefficient-order interop pinned by test: `opencv_radial` = **`[k1, k2, p1, p2, (k3)]`** (OpenCV/COLMAP order, 4 or 5 coefficients) — mirrors Addendum 8a.
- The DLT sign convention is verified against the ground-truth fixture: uncorrected signs placed points BEHIND cameras (`kRejectedNegativeDepth`); corrected formulas recover truth to `≤1e-6 m`.
- D5 with a single candidate is degenerate (median = own residual ⇒ always accepted); correct D5 requires ≥2 clean candidates so the global median ≈ 0 and the threshold stays at the 2px floor. Documented in the 50px-outlier test.
- Unused/dead helper removed (`CheckReprojectionAcceptance`) — the residual path is `ComputePointReprojectionError` + global median pass; avoids a misleading "acceptance" helper that never computed residuals.
- Lineage emission: REPLACED points are appended with fresh `point3d_id` values from a monotonically increasing counter (starts at 10001, `next_point_id++`); PRESERVED keeps the original v2 id. Note: for the closed-square fixtures (ids 1001–1004) there is no overlap; a synthetic v2 whose ids already reach ≥10001 is out of the current fixture scope.
- No Eigen outside `core/geometry/`; gates unchanged (see below).

### Gates / verification

- Build & tests: Debug and Release both **651/651** pass (15 new 8b tests + full suite; 636 → 651).
- `check_domain_types.py`: 2 pre-existing violations (unchanged). `check_dependencies.py`, `check_schemas.py`, `check_worker_boundary.py`, `check_arch_debt.py`, `check_rfc.py`: PASS.
- `check_constitution.py --base HEAD --rfc RFC-0002`: PASS (5 protected paths: the three `core/geometry/` headers + the two extended `engine/pipeline/quality/` files).

### 8c / 8d (NOT started)

8c (bundle adjustment → v4, fixed intrinsics, BA seed plumbing D6) and 8d (intrinsics refinement) remain future increments. 8c builds on the v3 output + the D5 gate predicate (`PassesReprojectionGate`) and the `ApplyOptimizedTrajectory` provenance pattern that 8b mirrors.

---

*P3-impl-8b implemented by opencode on 2026-09-06 (Debug + Release full suites green, all gates green). Increment 8b source files: `core/geometry/triangulation.h`, `tests/unit/test_triangulation.cpp`, `tests/CMakeLists.txt`.*

---

*P3-impl-8a implemented by opencode on 2026-09-05 (Debug + Release full suites green, all gates green). Increment 8a source files: `core/geometry/camera_model.h`, `core/geometry/reprojection.h`, `engine/pipeline/quality/quality_report.{h,cpp}`, `tests/unit/fixtures/closed_square_reconstruction.h`, `tests/unit/test_reprojection_geometry.cpp`, `tests/CMakeLists.txt`.*
