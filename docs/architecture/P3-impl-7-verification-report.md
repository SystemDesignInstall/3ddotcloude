# P3-impl-7 Verification Report — Optimized Trajectory → Reconstruction v2 (Pose-Only Feedback)

**Status:** VERIFIED — PASS
**Date:** 2026-09-03
**Author:** opencode
**Increment:** P3-impl-7 Phase 2 (D-RI-01…D-RI-04)
**Depends on:** Phase 1 (LC-1 `SetReconstructionStatus`) — ACCEPTED

---

## 1. Scope of this increment

Implemented the core **`ApplyOptimizedTrajectory`** seam (Mapping §12.1 / blocker-resolution CF-1)
that feeds a corrected (post-GTSAM) trajectory into a **new** P2.5 Reconstruction revision under
**Option A (pose-only feedback)**:

- **CF-1:** caller-supplied `geometry::SE3 reconstruction_from_trajectory`; the seam never infers a
  frame relationship from `coordinate_frame` strings. Same-frame requires explicit `Identity`;
  `alignment_resolved=false` ⇒ fail closed (typed error, nothing written).
- **LC-1:** the source Reconstruction row is superseded via `MetadataDb::SetReconstructionStatus`
  (Phase 1, already ACCEPTED). This increment produces the new document and verifies the
  insert + supersede persistence flow.
- **SC-1:** only `ReconImage.pose` of matched images is updated. `ReconPoint3D.xyz`/track,
  `ReconCamera`, and `detected` preserved verbatim. No re-triangulation, no BA, no point transform.

The seam is **header-only** (per Mapping §12.1 "header-only, canonical, backend-free — consistent
with `pose_graph_helpers.h` style"), so no `reconstruction_feedback.cpp` or `spatial_core` CMake
change was required. It reuses `MakeCameraPose` from `core/trajectory/pose_graph_helpers.h`
(Mapping §4.6 pseudo-C++ uses the same helper) and introduces no raw-Eigen tokens.

---

## 2. Files changed / created

| File | Change | Status |
|------|--------|--------|
| `core/trajectory/reconstruction_feedback.h` | **(new)** seam: `ReconstructionFeedbackInput`, `ReconstructionFeedbackResult`, `PoseFeedbackDetail`, `ApplyOptimizedTrajectory`, `ValidateOptimizedReconstruction`, `MakeReconPose` | Created (committed) |
| `tests/unit/test_reconstruction_feedback.cpp` | **(new)** test suite ($3) | Created |
| `tests/CMakeLists.txt` | register `unit/test_reconstruction_feedback.cpp` in `SPATIAL_UNIT_TESTS` (+1 line) | Modified |
| `core/storage/metadata_db.{h,cpp}` | `SetReconstructionStatus` — **Phase 1, not this increment** | Already ACCEPTED |

The header-only design intentionally produces **no** `reconstruction_feedback.cpp` and therefore
**no** change to `core/CMakeLists.txt`. No schema, no migration, no DB-table change.

---

## 3. Test matrix

**20 new tests** in `tests/unit/test_reconstruction_feedback.cpp` (17 from Mapping §14 + LC-1
DB-supersede checks + 2 robustness tests). All PASS in Debug and Release.

| # | Test | Verifies |
|---|------|----------|
| 1 | `JoinBothPresent` | node↔image joined by `frame_id`; optimized pose applied (identity write-back) |
| 2 | `JoinTrajNoImage` | node with no image → skipped, no phantom image created |
| 3 | `JoinImageNoTraj` | image with no node → original pose preserved |
| 4 | `JoinDetectedFalse` | `detected=false` image copied unchanged, never updated |
| 5 | `CoordinateFrameMismatch` | frames differ + `alignment_resolved=false` → throws; nothing written |
| 6 | `CoordinateFrameIdentity` | identity alignment → raw `T_trajectory_camera` written, numerically unmodified |
| 7 | `CoordinateFrameAligned` | `T_rc = T_rt * T_tc` correct under a caller-supplied non-identity SE(3) |
| 8 | `NoDoubleInversion` | golden numeric: `T_rc == T_rt * T_tc`, **≠** `T_rt * T_tc⁻¹`, **≠** `T_rt⁻¹ * T_tc` (non-trivial rotation) |
| 9 | `UnmatchedNodes` | mixed matched/unmatched → matched updated, unmatched preserved, order stable |
| 10 | `MissingFrameId` | empty `frame_id` on image (skip/keep) and on node (skip); continues |
| 11 | `DeterministicOutput` | identical inputs → identical content (modulo mandated instance UUID) |
| 12 | `CasHashDeterminism` | identical content → identical SHA-256 content address (dedupe property) |
| 13 | `SchemaValidation` | produced v2 doc has all schema `required` fields; `status="succeeded"`; `ValidateOptimizedReconstruction` |
| 14 | `OptionAPointsUnchanged` | `ReconPoint3D.xyz`/track, `ReconCamera`, `detected` byte-identical; only matched detected poses change |
| 15 | `RevisionSemantics` (DB) | new `reconstruction_id`; insert new row + supersede old row; source row `"superseded"`, new row active |
| 16 | `RevisionSemanticsSucceedsInSameScene` (DB) | exactly one active (`succeeded`) reconstruction per scene after supersede |
| 17 | `ProvenanceLineage` | `backend.name="spatial_optimizer"`; hashes include OptimizationResult + source Reconstruction; optimizer detail in `backend_specific_json`; no self-reference |
| 18 | `EndToEnd` | full chain: new id, succeeded, points/cameras identical, poses corrected, provenance, validation |
| 19 | `EmptyOptimizationThrows` | empty `optimized_nodes` → `std::invalid_argument` |
| 20 | `SourceIsNeverMutated` | seam leaves the source Reconstruction byte-identical |

LC-1 lifecycle transition tests (`SetStatusSucceededToSuperseded`, terminal reject, missing row,
read-only, persist-after-reopen) were added in **Phase 1** (`tests/unit/test_reconstruction.cpp`).

---

## 4. CF-1 / LC-1 / SC-1 verification

- **CF-1** — seam signature takes `geometry::SE3 reconstruction_from_trajectory` directly; no
  `FrameGraph`, `FrameId`, or `CoordinateFrame` appears in the header; no `coordinate_frame` string
  is read to choose the transform (Tests #5–#8). Same-frame declared by `Identity` (Test #6);
  fail-closed when `alignment_resolved=false` (Test #5). Reuses `MakeCameraPose` (no SE(3)
  inversion of the corrected pose; `T_rc = T_rt * T_tc`, Test #8).
- **LC-1** — persistence flow exercised: `AddReconstruction` (new row) + `SetReconstructionStatus`
  (old row → `"superseded"`); `QueryLatestReconstructionByScene` repoints to the new active row
  (Tests #15–#16). Uses the Phase-1 `SetReconstructionStatus`, whose own 7 tests remain in
  `test_reconstruction.cpp`.
- **SC-1** — `ReconPoint3D.xyz`/`track`, `ReconCamera`, `detected`, `session_ids`, `scene_id`,
  `coordinate_frame` all preserved with `==` equality against the source (Test #14). Only
  `ReconImage.pose` of matched, detected images is rewritten.

---

## 5. Determinism & provenance

- **Determinism (Mapping §11):** `created_at_ns` is carried from `optimization_result.created_at_ns`;
  provenance timing fields stay zero; `input_artifact_hashes` sorted ascending; output `images[]`
  kept in source order. Identical inputs ⇒ identical content bytes ⇒ identical SHA-256 content
  address (Tests #11–#12). The instance `reconstruction_id` is a UUIDv4 (not content-derived)
  and is the only field allowed to differ per run.
- **Provenance (Mapping §8):** `backend.name="spatial_optimizer"`, version = optimizer version,
  `backend_specific_json = {"optimizer":"...","optimization_result_id":"...","option":"A"}`;
  `input_artifact_hashes` inherit the source chain and append the caller-supplied OptimizationResult
  and source Reconstruction CAS hashes; chain is acyclic (no self-reference, Test #17).

---

## 6. Schema validation

- Produced document serialized to a v2 shape (matching `ReconstructionToJson`) and checked against
  the root `required` list of `schemas/json/reconstruction.schema.json` (`schema_version`,
  `reconstruction_id`, `scene_id`, `session_ids`, `coordinate_frame`, `provenance`, `cameras`,
  `images`, `points3D`), with `status="succeeded"` and non-empty identity fields (Test #13).
- `ValidateOptimizedReconstruction` provides the deterministic structural/consistency check used by
  the seam's consumers (Mapping §12.3).

---

## 7. Debug / Release results

| Configuration | Suite result |
|---------------|--------------|
| **Debug** | full `ctest -C Debug`: **591/591 PASS** (571 baseline + 20 new) |
| **Release** | full `ctest -C Release`: **591/591 PASS** (571 baseline + 20 new) |

---

## 8. Architecture / boundary / schema gates

| Check | Result |
|-------|--------|
| `check_dependencies.py` | **PASS** (6 packages registered, permissive licenses) |
| `check_schemas.py` | **PASS** (18 JSON schemas, 7 migrations, schema.sql present) |
| `check_worker_boundary.py` | **PASS** (25 files, no direct DB/scene access from workers) |
| `check_arch_debt.py` | **PASS** (150 files, no debt markers) |
| `check_rfc.py` | **PASS** (39 ADRs, 8 RFCs, index consistent) |
| `check_domain_types.py` | **FAIL (pre-existing, NOT a Phase-2 regression)** — only `core/trajectory/pose_graph_helpers.h:48,69` (6c debt, `Eigen::Vector3d`). The new `reconstruction_feedback.h` introduces **zero** raw-Eigen tokens (verified by scan). |

The domain-types failure is the previously-documented, pre-existing 6c debt, unchanged from Phase 1
and unrelated to this increment. `check_constitution.py` is not part of this increment's counted
gates (it requires a `--base`/`--rfc` governance ref and is out of scope for this seam change).

### Boundary audit

- No change to `core/reconstruction/reconstruction.h`, `core/trajectory/*` canonical types,
  `core/geometry/*`, `core/coordinates/*`, `core/scene/frame.h`.
- No change to `adapters/gtsam/*`, `adapters/colmap/*`.
- No change to any JSON schema or DB migration.
- The seam is pure/canonical: no `MetadataDb`, `FrameGraph`, GTSAM, or COLMAP types appear in the
  header.

---

## 9. Deferred / out of scope (per blocker-resolution §8)

1. **Production orchestration pipeline stage** (`engine/pipeline/<new>_stage.*`) that reads inputs,
   resolves frame alignment, calls the seam, and persists — marked "Design P3-impl-7 orchestration
   (not the seam itself)" in blocker-resolution §8 item 342; the seam + persistence correctness are
   verified here via the DB tests. Deferred to the orchestration increment.
2. 3D-point recomputation / re-triangulation / full BA (future).
3. FrameGraph-based frame registration from `coordinate_frame` strings (future pipeline-stage
   concern).
4. Multi-session alignment, map merge, uncertainty propagation to points.
5. Loop-closure implementation (the other `impl-7a/7b/7c` thread).

---

## 10. Verdict

**PASS.** The `ApplyOptimizedTrajectory` seam (CF-1), the LC-1 supersede persistence path, and the
SC-1 Option-A scope are implemented and verified:

- 20 new tests PASS (join cases, CF-1 direction/fail-closed/identity, SC-1 preservation, determinism,
  CAS hash, schema validation, provenance lineage, DB revision semantics, robustness);
- Debug and Release full suites **591/591 PASS**;
- all architecture/boundary gates PASS except the pre-existing, unrelated 6c domain-types debt
  (unchanged; the new seam adds no raw-Eigen tokens);
- no schema, migration, canonical-type, adapter, geometry, or coordinates change.

**STOP.** Do not begin the next increment (P3-impl-8 / orchestration stage) until this report is
accepted.

---

*Produced by opencode on 2026-09-03. No code beyond the listed P3-impl-7 files was changed.*

---

# Addendum — Phase 3 (P3-impl-7c) E2E orchestration + engine-stage DB persistence

**Status:** VERIFIED — PASS
**Date:** 2026-09-04
**Increment:** P3-impl-7 Phase 3 (spec: `P3-impl-7-phase3-e2e-orchestration-spec.md`)
**Milestone:** final functional increment before Release verification

## 1. Scope of the Phase-3 increments

Phase 3 assembled and proved the full production orchestration path from a verified
visual/metric loop closure through a persisted, optimizer-corrected reconstruction:

```
7a Candidate Generation → 7b Geometric Verification → Metric Loop Closure
  → PoseGraphEdge → TrajectoryOptimizer seam → GTSAM
  → ApplyOptimizedTrajectory → new Reconstruction + DB supersede
```

and the two principle-proving E2E tests:

```
metric basis present → loop edge → GTSAM → drift reduced          (INV-2)
metric basis absent  → NO loop edge → optimizer unchanged         (INV-3)
```

The **final increment** (this report) added the **engine-stage DB persistence**:
`Verified LoopClosure → PoseGraphRow → (seam) OptimizationResultRow` through a real
engine orchestration stage, with idempotent (INSERT OR REPLACE) persistence and the
real UPDATE path for `LoopClosure.spatial_separation_m`.

## 2. Files changed / created (final increment)

| File | Change | Status |
|------|--------|--------|
| `engine/pipeline/loop_closure_optimize_pipeline.{h,cpp}` | **(new)** orchestration-runner stage: closure → `LoopClosureToPoseGraph` → `PoseGraphRow` (Upsert) → seam optimizer → `OptimizationResultRow` (Upsert), closure spatial_separation_m UPDATE | Created |
| `core/storage/metadata_db.{h,cpp}` | **(new)** idempotent `UpsertPoseGraph` / `UpsertLoopClosure` / `UpsertOptimizationResult` (INSERT OR REPLACE) | Modified (additive) |
| `engine/CMakeLists.txt` | register `pipeline/loop_closure_optimize_pipeline.cpp` | Modified (additive) |
| `tests/CMakeLists.txt` | register `unit/test_loop_closure_optimize_pipeline.cpp` (gated, links GTSAM adapter for the real seam) | Modified (additive) |
| `tests/unit/test_loop_closure_optimize_pipeline.cpp` | **(new)** persistence + idempotency + INV-3 tests | Created |

Pre-existing `AddPoseGraph` / `AddLoopClosure` / `AddOptimizationResult` are unchanged;
the new `Upsert*` methods coexist (used only by the orchestration path).

## 3. Test matrix (final increment)

**3 new tests** in `tests/unit/test_loop_closure_optimize_pipeline.cpp`:

| Test | Verifies |
|------|----------|
| `PersistsGraphOptAndClosureSpatial` | real engine path writes `PoseGraphRow` (1 metric + 4 odometry edges), `OptimizationResultRow` (`converged`), and `LoopClosureRow` with D2 `spatial_separation_m == \|\|p_target − p_source\|\|` (0.453 m); stable `graph_id`/`result_id` |
| `RepeatedProcessingIsIdempotent` | re-processing the SAME closure (same `closure_id`/trajectory) keeps exactly ONE `PoseGraphRow`/`OptimizationResultRow`/`LoopClosureRow`, stable identities, no PK conflict, no duplicate/contradictory canonical rows |
| `UndeclaredBasisNoOptButClosurePersisted` | INV-3: undeclared metric basis → `ran=false`, NO pose graph, NO optimization; closure still persisted for audit with D2 spatial filled |

Prior Phase-3 rows already in the suite: `spatial_gtsam_tests` (INV-2 genuine-metric
drift reduction, INV-3 negative, INV-4 seam→ApplyOptimizedTrajectory→reconstruction→DB
supersede, metric-edge builder, D5/D6/D3/D4, seam T7) and `spatial_loop_closure_stage_tests`
(T4/T5/D2/INV-1/INV-3).

## 4. Invariant confirmation (final increment)

- **PoseGraphRow persisted via the engine orchestration path**, not merely DB-called in a
  test: the stage assembles the full graph (odometry edges + one metric loop edge) and
  `UpsertPoseGraph`s it.
- **OptimizationResultRow persisted via the engine orchestration path**: the stage runs the
  injected `TrajectoryOptimizer` seam over the graph and `UpsertOptimizationResult`s the
  canonical result.
- **`LoopClosure.spatial_separation_m` real UPDATE path**: `UpsertLoopClosure` (INSERT OR
  REPLACE) overwrites the row with the recomputed diagnostic. Semantics preserved:
  `spatial_separation_m = \|\|p_target − p_source\|\|` from trajectory nodes; it is NEVER the
  metric loop translation; no identity/zero fabricated measurement (INV-1); undeclared basis
  ⇒ no metric edge (INV-3).
- **Idempotency / duplicate behavior**: every persisted canonical row carries a stable v5
  identity (`graph_id`/`result_id` = f(trajectory_id, closure_id)). Repeated processing of the
  same closure overwrites the same rows; no duplicate/contradictory canonical rows; no repeated
  side effect on already-superseded reconstructions (reconstruction supersede itself is
  exercised in the INV-4 test and uses the existing `SetReconstructionStatus` transition).

## 5. Debug / Release results

| Configuration | Suite result |
|---------------|--------------|
| **Debug** | full `ctest -C Debug`: **602/602 PASS** (601 prior + 1 new orchestration target) |
| **Release** | full `ctest -C Release`: **602/602 PASS** |

## 6. Architecture / boundary / domain gates

| Check | Result |
|-------|--------|
| `check_domain_types.py` | **2** violations (pre-existing `pose_graph_helpers.h:49,70` 6c debt, UNCHANGED). New engine stage + tests add **zero** raw-Eigen tokens (canonical types + `ParseUuid`/`FormatUuid`/`GenerateUuidV5` only). |
| engine→GTSAM boundary | The orchestration stage includes **only** the `core::TrajectoryOptimizer` seam; no GTSAM type/header in `engine/`. The real GTSAM adapter is injected by the test. |
| `check_schemas.py` / `check_rfc.py` / `check_dependencies.py` / `check_worker_boundary.py` / `check_arch_debt.py` | unchanged (no schema/migration/architecture change in this increment) |

## 7. Verdict

**PASS.** The final functional increment completes P3-impl-7 Phase 3:

- real engine orchestration stage persists `PoseGraphRow` and `OptimizationResultRow` and
  provides the real UPDATE path for `LoopClosure.spatial_separation_m` (idempotent, stable
  identities — no duplicates/contradiction on repeated processing);
- INV-1 / INV-2 / INV-3 / INV-4 all proven on the real orchestration path;
- Debug and Release full suites **602/602 PASS**;
- `check_domain_types == 2` (unchanged pre-existing debt, no new raw-Eigen);
- working tree verified; diff reviewed.

**STOP.** P3-impl-7 is COMPLETE. Do not begin any subsequent P3 increment; decide the next
architectural milestone separately.
