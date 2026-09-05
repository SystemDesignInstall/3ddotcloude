# P3-impl-7 — Phase 3: E2E Orchestration Specification

**Status:** DRAFT — design/specification only. **ZERO code changes.**
**Date:** 2026-09-03
**Scope:** The full production orchestration: Verified `LoopClosure` → `PoseGraphEdge` → `AssemblePoseGraph` → `TrajectoryOptimizer` (GTSAM) → `OptimizationResult` → `ApplyOptimizedTrajectory` → new `Reconstruction` → DB persistence. Converts the four mandatory Phase-3 invariants into concrete, testable acceptance criteria, and specifies the **consumer side** of the Phase-2 seam (which the readiness doc stops short of).

---

## 0. Relation to existing documents

| Document | Relationship |
|----------|--------------|
| `P3-impl-7c-implementation-readiness.md` | Foundational technical design: seams (§7.1 `TrajectoryOptimizer`, §7.2 engine stage), `BuildLoopClosureEdge` rework (§8), deterministic info matrix (§9 / D3), metric binding (§10 / D5), F-only closures (§11), E2E criteria (§12: D-7c-12 / D-7c-13), test matrix (§13), decisions D1–D6 (§14). **This doc extends it; it does not restate it.** |
| `P3-impl-7-implementation-mapping.md` | CF-1 / LC-1 / SC-1 decisions and seam contract. |
| `P3-impl-7-blocker-resolution.md` | §8 item 342 defers the *production orchestration pipeline stage* (not the seam). **Phase 3 closes that deferral.** |
| `P3-impl-7-verification-report.md` (Phase 2) | Seam (`reconstruction_feedback.h`) verified; `ApplyOptimizedTrajectory` proven at unit level. **Phase 3 proves it on the real orchestration path.** |

The readiness doc §7.2 wires **up to** persisting `PoseGraph`/`OptimizationResult`. The gap Phase 3 fills: the **downstream consumer side** — GTSAM `OptimizationOutput` → `ApplyOptimizedTrajectory` → new `Reconstruction` → `SetReconstructionStatus` supersede — and the two quantitative E2E proofs run through the real DB/artifact path.

---

## 1. Mandatory invariants (Phase-3 acceptance criteria)

The four invariants, each with a concrete, falsifiable acceptance test. Source of truth: user-mandated Phase-3 scope.

| # | Invariant | Falsifiable criterion | Governing test |
|---|-----------|-----------------------|----------------|
| **1** | **No identity loop edge.** A verified *visual-only* closure (no metric relative pose) produces **no** `PoseGraphEdge`. A real metric closure carries a **real** relative pose (`relative_position_xyz` ≠ `{0,0,0}`), never identity. | `BuildMetricLoopClosureEdge` returns `std::nullopt` when no measurement is provided; the real orchestration path never emits an edge with identity relative pose. `BuildLoopClosureEdge`'s current hard-coded identity (`pose_graph_helpers.h:336-337`) is **not** the implementation used for real closures. | T6, neg-E2E (INV-3) |
| **2** | **Real metric E2E drift reduction.** Trajectory BEFORE → loop constraint → GTSAM → trajectory AFTER, quantitatively `error_after < error_before`. | `RMSE_position,after < RMSE_position,before` with margin ≥ 30% (D-7c-12), on the **same** ground-truth knots, loop edge being the **only** difference. | pos-E2E (INV-2) |
| **3** | **Negative E2E (no fabrication regression).** Uncalibrated/undeclared metric basis ⇒ closure persisted but **no** metric edge ⇒ optimizer unchanged. | No `PoseGraphEdge` of type `loop_closure`; the optimizer never runs on loop edges; output trajectory nodes numerically identical to input within machine epsilon; run exits `OK`. | neg-E2E (INV-3) |
| **4** | **`ApplyOptimizedTrajectory` proven on the real orchestration path** (GTSAM output → seam → DB → `Reconstruction`), not merely unit-called. | The E2E drives GTSAM `OptimizationOutput.optimized_nodes` through `ApplyOptimizedTrajectory`, produces a **new** `Reconstruction` revision whose matched `ReconImage.pose` differ from the source, persists it via `MetadataDb`, and supersedes the prior revision — all on one run. | pos-E2E (INV-4) |

---

## 2. Exact data flow (the pipeline Phase 3 assembles)

```
Verified LoopClosure (loop_closure.h, status="accepted")
   │  carries source_frame_id / target_frame_id, spatial_separation_m,
   │  confidence, provenance (D5: (R_ess, t̂_ess) measured geometry)
   ▼
loop_closure_to_pose_graph (engine stage, P3-impl-7c §7.2)
   │  - resolve frames → trajectory pose nodes (frame_id bridge, D-TRJ-10)
   │  - metric-eligibility check (metric_basis provenance-derived, §5.1 / D4)
   │  - eligible  → BuildMetricLoopClosureEdge (real metric relative pose,
   │                deterministic info matrix)  ── produces ONE PoseGraphEdge
   │  - ineligible → NO edge (closure persists as verified-visual only)
   ▼
PoseGraph (pose_graph.h) = { odometry edges (AssemblePoseGraph) + metric loop edge }
   ▼
TrajectoryOptimizer seam (optimizer.h §7.1, D-7c-5)
   ▼
GTSAM adapter (gtsam_optimizer_adapter.h): OptimizationInput → optimize() → OptimizationOutput
   ▼
OptimizationResult (optimization.h: OptimizationResult + OptimizedPoseNode[] )
   ▼
ApplyOptimizedTrajectory (reconstruction_feedback.h:103)   ←── Phase-2 seam, now on the real path
   │  T_reconstruction_camera = reconstruction_from_trajectory * T_trajectory_camera (CF-1)
   │  Option A: only matched ReconImage.pose updated; points/cameras/detected byte-identical (SC-1)
   ▼
NEW Reconstruction revision (v2)  (reconstruction_id regenerated, status="succeeded", provenance lineage)
   ▼
MetadataDb persistence:
   - AddReconstruction (metadata_db.h:450) inserts the v2 row
   - SetReconstructionStatus(prev_id, "superseded") (metadata_db.h:464) supersedes v1
```

---

## 3. Stage-by-stage contract (what each hop must guarantee)

### 3.1 Verified LoopClosure → PoseGraphEdge (INV-1)

- **Input:** `LoopClosure` (`core/trajectory/loop_closure.h:36`) with `status == "accepted"`.
- **Gate:** metric eligibility is **provenance-derived** (`metric_basis` on the trajectory root, D4; `LoopClosure` stores measured `(R_ess, t̂_ess)` per D5). Absent/undeclared basis ⇒ **no edge** (INV-3).
- **Edge construction:** `BuildMetricLoopClosureEdge` (P3-impl-7c §8) — requires a metric-relative-pose measurement parameter (no default); returns `std::nullopt` when absent. This **replaces** the current `BuildLoopClosureEdge` fabricated identity (`pose_graph_helpers.h:336-337`) as the real-path implementation.
- **`spatial_separation_m` (exact, D2):** computed in `loop_closure_to_pose_graph` as `||p_t_ww − p_s_ww||` from the trajectory nodes at the closure's frame ids. It is a **diagnostic/quality field** — never the edge translation, never `||relative_position_xyz||`. Filled for provenance; also fixes the `loop-closure.schema.json`-required field currently stored as `0.0` (`engine/pipeline/loop_closure_verification.cpp:48,114`).
- **Information matrix (exact, D3):** the `PoseGraphEdge.information_matrix_6x6` (6×6 row-major, `pose_graph.h:42`) is the deterministic confidence-derived diagonal from the five verifier-quality inputs (§9 formula): `(1/σ_t²)×3, (1/σ_r²)×3`. Validated by `ValidateInformationMatrix` (`pose_graph_helpers.h:96`). No magic `50.0`/`300.0` constants in the real path. **This is the same matrix `ApplyOptimizedTrajectory` does not touch** — it lives on the edge; the seam consumes only pose nodes.
- **Counters on `PoseGraph`:** `loop_closure_edge_count` (`pose_graph.h:71`) must equal the number of metric edges emitted (0 in the negative case).

### 3.2 PoseGraph → TrajectoryOptimizer (INV-2, INV-3)

- `AssemblePoseGraph` (`pose_graph_helpers.h:147`) builds the odometry-only base graph; the metric loop edge is added on top. The optimizer runs **only** when metric edges exist.
- Contract split for the two E2E runs:
  - **Positive (INV-2):** base odometry graph + exactly **one** metric loop edge → GTSAM.
  - **Negative (INV-3):** base odometry graph, **zero** loop edges. The orchestrator detects "no metric edges" and **skips the optimizer** — output nodes are the input nodes (proved within machine epsilon), run exits `OK` with `metric-basis: absent → no edge`. This proves no fabrication regression: an uncalibrated closure never silently "fixes" trajectory drift.

### 3.3 TrajectoryOptimizer → GTSAM (INV-2)

- The seam `TrajectoryOptimizer::optimize(const OptimizationInput&)` (`P3-impl-7c §7.1`; interface header-only `core/trajectory/optimizer.h`) is implemented by the GTSAM adapter wrapping the existing free `optimize()` (`gtsam_optimizer_adapter.cpp:209`).
- `OptimizationInput` fields: `trajectory`, `trajectory_nodes`, `graph`, `graph_nodes`, `graph_edges`, `options`, `anchor_prior` (`gtsam_optimizer_adapter.h:45-53`).
- `OptimizationOutput`: `OptimizationResult` + `std::vector<OptimizedPoseNode>` (`gtsam_optimizer_adapter.h:56-59`).
- `OptimizedPoseNode` (`optimization.h:42`): `frame_id`, `timestamp_ns`, `sequence_index`, `position_xyz`, `rotation_xyzw`, covariance fields — exactly the join key `ApplyOptimizedTrajectory` needs.
- **Immutability contract:** `optimize()` takes `OptimizationInput` by value; original trajectory bytes unchanged (already proven in 6c, `DriftReductionWithLoop` `:1539-1629`). The orchestration re-asserts this at the DB/CAS boundary.

### 3.4 OptimizationOutput → ApplyOptimizedTrajectory (INV-4) ← NEW spec content

This hop is the **primary addition** of this spec over the readiness doc.

- **Input construction** (`reconstruction_feedback.h:46` `ReconstructionFeedbackInput`):
  - `source` = the current persisted `Reconstruction` (v1), read from the DB via `QueryLatestReconstructionByScene` (`metadata_db.h:451`) / `FindReconstructionsByScene` (`:453`).
  - `trajectory` / `trajectory_nodes` = the trajectory that produced the reconstruction (cross-check).
  - `optimization_result` = the `OptimizationResult` returned by GTSAM.
  - `optimized_nodes` = the `OptimizationOutput.optimized_nodes` **directly from GTSAM** (no separate re-derivation).
  - `reconstruction_from_trajectory` = caller-supplied `T_reconstruction_trajectory` (CF-1); never read from `coordinate_frame` strings; same-frame = explicit `Identity`; `alignment_resolved=false` ⇒ fail-closed (no write).
  - `source_reconstruction_cas_hash` / `optimization_result_cas_hash` = lineage hashes appended to provenance (`reconstruction_feedback.h:57-58`).
- **Guarded preconditions** (`reconstruction_feedback.h:106-116`): empty `optimized_nodes` ⇒ throw; `!alignment_resolved` ⇒ throw. In production orchestration these must be pre-checked to fail the stage cleanly before any DB write.
- **Output:** `ReconstructionFeedbackResult.reconstruction` = a **new revision (v2)**:
  - `reconstruction_id` regenerated (UUIDv4) (`:137`); `status="succeeded"` (`:138`); `created_at_ns` = optimization result's (`:142`).
  - Provenance lineage → `backend.name="spatial_optimizer"`, `input_artifact_hashes` inherits + appends both CAS hashes, sorted (`:144-180`).
  - Option A: only matched `ReconImage.pose` replaced from `MakeReconPose(T_reconstruction_camera)` (`:216`); unmatched/missing/not-detected images preserved byte-identical (SC-1).
  - Validated by `ValidateOptimizedReconstruction` (`:230`) against `reconstruction.schema.json` before persistence.

### 3.5 ApplyOptimizedTrajectory → DB persistence (INV-4) ← NEW spec content

- `MetadataDb::AddReconstruction(v2_row)` (`metadata_db.cpp:1830`) inserts the v2 `ReconstructionRow` (`metadata_db.h:186-193`).
- `MetadataDb::SetReconstructionStatus(v1_id, "superseded")` (`metadata_db.cpp:1932`) marks the prior revision superseded.
- Result: `QueryLatestReconstructionByScene(scene_id)` returns **v2**; `FindReconstructionsByScene` returns both, v1 `"superseded"`, v2 `"succeeded"` — the revision chain is auditable.
- The `ReconstructionRow.document_json` holds the v2 CAS payload; the CAS hashes referenced in provenance point at the artifacts actually consumed/produced (no dangling lineage).

---

## 4. E2E test design

Both E2E tests run the **real** orchestration path: DB + CAS + GTSAM adapter + seam, i.e. they exercise `loop_closure_to_pose_graph` → `TrajectoryOptimizer` → `ApplyOptimizedTrajectory` → `AddReconstruction`/`SetReconstructionStatus`. They reuse the deterministic closed-square drift fixture already in `test_gtsam_adapter.cpp` (`DriftReductionWithLoop :1539`), but drive it through the full stack.

### 4.1 INV-2 + INV-4 — Positive E2E: drift reduction + real reconstruction

**Precondition:** metric basis declared on the trajectory root (provenance-derived, D4); a real metric closure with `has_relative_pose=true`.

1. `AssemblePoseGraph` → odometry-only base graph (drift accumulates at the final node).
2. Verified metric `LoopClosure` → `BuildMetricLoopClosureEdge` → exactly **one** `PoseGraphEdge` (`relative_position_xyz` ≠ `{0,0,0}`, deterministic info matrix, `ValidateInformationMatrix` passes).
3. `TrajectoryOptimizer` (GTSAM) → `OptimizationOutput.optimized_nodes`.
4. `ApplyOptimizedTrajectory` (INV-4) → v2 `Reconstruction`; `AddReconstruction`; `SetReconstructionStatus(v1, "superseded")`.

**Assertions:**

| Invariant | Assertion |
|-----------|-----------|
| INV-2 | `RMSE_position,after(ground_truth) < RMSE_position,before(ground_truth)`, margin ≥ 30% (D-7c-12), same GT knots, loop edge the only difference. Reported numerically (e.g. `RMSE 0.040 m → 0.012 m`). |
| INV-4 | v2 row present and latest; v1 `"superseded"`; at least one matched `ReconImage.pose` in v2 differs from v1; points/cameras/detected byte-identical (SC-1 hash check). |
| INV-1 (positive) | the single emitted loop edge has non-identity `relative_position_xyz`. |
| seams | GTSAM types never appear in `engine/`; engine dispatches only through `TrajectoryOptimizer` interface (D-7c-5). |

### 4.2 INV-3 — Negative E2E: undeclared metric basis ⇒ no fabrication

**Precondition:** same trajectory but **undeclared** metric basis (COLMAP-only / no calibration, or closure fails cheirality/consistency guard D-7c-9).

1. Verified closure persisted (audit) with `has_relative_pose=false` or non-eligible metric basis.
2. `loop_closure_to_pose_graph` computes `spatial_separation_m` (diagnostic) but emits **no** metric edge.
3. Result document states `metric-basis: absent → no edge`; run exits `OK`.

**Assertions:**

| Invariant | Assertion |
|-----------|-----------|
| INV-3 | `PoseGraph.loop_closure_edge_count == 0`; zero `loop_closure` edges reach the optimizer; **optimizer skipped**; output trajectory nodes numerically identical to input within machine epsilon (asserted by comparing output nodes to input nodes). |
| INV-1 | `BuildMetricLoopClosureEdge` → `std::nullopt` (no measurement ⇒ no edge). |
| no regression | the closure record remains persisted as verified-visual-only; nothing fabricated downstream. |

---

## 5. Exact `spatial_separation_m` and information-matrix contract (consolidated)

| Field | Where computed | Semantics (exact) | Verification |
|-------|----------------|--------------------|--------------|
| `LoopClosure.spatial_separation_m` | `loop_closure_to_pose_graph` (D2) | `||p_t_ww − p_s_ww||` (trajectory nodes, W). Diagnostic/quality; **never** the edge translation, never `||relative_position_xyz||`. | T5; E2E asserts it is demonstrably different from the edge translation. |
| `PoseGraphEdge.information_matrix_6x6` | `BuildMetricLoopClosureEdge` (D3) | Deterministic diagonal `(1/σ_t²)×3, (1/σ_r²)×3` from 5 verifier-quality inputs (formula §9). | T8; `ValidateInformationMatrix` gate; no `50.0`/`300.0` provenance in real path. |
| `AnchorPriorConfig.information_scale` (`gtsam_optimizer_adapter.h:40`, default `1e6`) | adapter boundary | Configurable default, **not** normative; used only for the node-0 prior, unrelated to loop-edge info. | unchanged |

**Distinction that must never couple:** `spatial_separation_m` is a scalar diagnostic; `relative_position_xyz` (edge translation) is the metric measurement `λ·t̂_ess` (D5/D6); `information_matrix_6x6` is the uncertainty. The seam's `ReconPose` is derived **only** from the optimized trajectory poses via CF-1 — never from `spatial_separation_m` or the info matrix.

---

## 6. Raw-Eigen / domain-type debt constraint

- **Do not touch** the pre-existing 6c debt at `pose_graph_helpers.h:48,69` (raw `Eigen::Vector3d`). It is out of Phase-3 scope; `check_domain_types` still reports those two lines and **must not report more**.
- The new orchestration code (engine stage + seam consumption) must follow the Phase-2 seam pattern: **zero raw-Eigen tokens**, keeping raw Eigen inside `core/geometry/` and `adapters/`. Re-verify the `check_domain_types` violation count is **unchanged** after Phase-3 implementation.
- If Phase 3 forces a change to `pose_graph_helpers.h`, change **only** what orchestration requires (`BuildLoopClosureEdge` → `BuildMetricLoopClosureEdge` per §8 of the readiness doc) and re-verify the debt count.

---

## 7. Boundaries (NOT in Phase 3)

- No new optimizer backend; no GTSAM/COLMAP/geometry/coordinates/frame-graph changes.
- No schema migration / DB-table change beyond the status-UPDATE path already flagged (readiness §10).
- No canonical-type change beyond the already-specified `LoopClosure` payload extension (D5) and `metric_basis` (D4).
- No point transform / triangulation / bundle adjustment (Option A: camera poses only).
- No multi-session / AI / Insta360 / GeoLibre work.

The 3D-point recompute, `FrameGraph`-based alignment, and multi-session items remain deferred (as in the Phase-2 report).

---

## 8. Implementation sequence (for GO, from readiness §15, now with the consumer side)

1. T1/T2 math + guard unit tests; T3/T4 non-metric; T5 `spatial_separation_m`; T6 edge builder; T8 info matrix.
2. Core seam `TrajectoryOptimizer` (§7.1); adapter wraps existing free `optimize()`.
3. D4 `metric_basis` schema + `LoopClosure` payload (D5).
4. Core edge builder `BuildMetricLoopClosureEdge` (§8) + `spatial_separation_m` fix (D2).
5. D3 info matrix (§9).
6. Engine stage `loop_closure_to_pose_graph` (§7.2) + status UPDATE.
7. **Consumer side (this spec §3.4-§3.5):** wire `ApplyOptimizedTrajectory` + `AddReconstruction`/`SetReconstructionStatus` into the engine stage.
8. **E2E** (this spec §4): positive (INV-2+INV-4) and negative (INV-3).
9. Gates: `check_constitution.py --base <7b-commit>` (must document base for the counted gates), `check_dependencies`/`check_schemas`/`check_worker_boundary`/`check_arch_debt`/`check_rfc`; full ctest Debug+Release; `check_domain_types` count unchanged.

**This document makes no code changes.**

---

## 9. Owner review checklist (maps invariants → evidence)

1. **INV-1 no identity edge** — neg-E2E `loop_closure_edge_count==0`; `BuildMetricLoopClosureEdge` `std::nullopt` on absent measurement; real path never identity (`pose_graph_helpers.h:336-337` not used for real closures).
2. **INV-2 real drift reduction** — pos-E2E `RMSE_after < RMSE_before` (≥30%), same GT knots, loop edge the only difference.
3. **INV-3 negative proof** — no metric edge, optimizer skipped, nodes numerically unmodified, run `OK`.
4. **INV-4 seam on real path** — v2 `Reconstruction` from real GTSAM output, matched poses differ, DB supersede via `SetReconstructionStatus`, points/cameras/detected byte-identical.
5. **Exact `spatial_separation_m`/info handling** — D2/D3 as consolidated §5; never coupled to seam poses.
6. **No raw-Eigen debt growth** — `check_domain_types` count unchanged.

On GO, implement in the §8 order. No code has been changed by this document.
