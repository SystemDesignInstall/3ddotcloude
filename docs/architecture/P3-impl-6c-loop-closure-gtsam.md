# P3-impl-6c — Loop Closure → Pose Graph → GTSAM End-to-End (Drift Reduction)

**Status:** Implemented (see verification report)
**Date:** 2026-08-31
**Depends on:** ADR-P3-GTSAM-001 (accepted), P3-impl-6b (GTSAM adapter, 537/537 PASS), P3-impl-5 types
**Scope:** End-to-end proof that a verified loop-closure constraint, added to a canonical PoseGraph, measurably reduces trajectory drift when optimized by GTSAM.

---

## 0. Objective

Prove, end to end and **without** touching any optimizer internals, that:

> A real loop closure → canonical PoseGraph `loop_closure` edge → existing GTSAM `optimize()`
> yields a corrected trajectory whose drift is **reduced** relative to the same pose graph without the loop.

Ground truth is used only as a **measurement oracle** (to quantify drift reduction); it is never
fed to the optimizer and never presented as optimizer output.

## 1. Architecture Rule Compliance

Per the 6c guardrail (P3 spec §32), 6c must NOT introduce new architectural surface. This increment
changes **none** of the protected contracts — it only *produces* and *consumes* existing canonical
types:

| Protected contract | Change? |
|--------------------|---------|
| `PoseGraph` / `PoseGraphNode` / `PoseGraphEdge` | None — types unchanged |
| `Trajectory` / `TrajectoryPoseNode` | None — only consumed; original never mutated |
| `LoopClosureCandidate` / `LoopClosure` | None — used as-is |
| `OptimizationResult` / `OptimizedPoseNode` | None — produced by existing adapter |
| Coordinate-frame semantics (T_trajectory_camera) | None |
| Artifact identity / scene / capability / scheduler | None — `ArtifactStore.put` on existing artifact types only |
| `spatial_gtsam_adapter.optimize()` | None — reused verbatim |

New code is **header-only domain helpers** in `core/trajectory/pose_graph_helpers.h`. These are pure
canonical-type builders (no optimizer types, no GTSAM includes), so they neither widen the adapter
boundary nor pull GTSAM into `core/`.

## 2. Deliverables

| File | Kind | Purpose |
|------|------|---------|
| `core/trajectory/pose_graph_helpers.h` | New (header-only) | Canonical transform math, info-matrix validation, pose-graph assembly, loop-closure pipeline (candidate → verify → edge) |
| `tests/unit/test_gtsam_adapter.cpp` | Extended | +6 helper unit tests, +2 E2E tests (total 39) |
| `docs/architecture/P3-impl-6c-verification-report.md` | New | Measured results |

## 3. Canonical Transform Helpers (domain code)

The adapter boundary must NOT re-implement transform math (ADR-007 / D-PG-04). We provide it here:

- `MakeCameraPose(position_xyz, rotation_xyzw)` — builds `T_trajectory_camera` as `SE3` from
  canonical fields (scalar-last quaternion normalized).
- `PoseFromNode(node)` — convenience wrapper.
- `RelativePoseBetween(a, b)` — computes **`T_ab = Ta^{-1} * Tb`**, matching the `T_source_target`
  convention of `PoseGraphEdge`. Unit-tested for identity, inverse, and translation deltas.
- `PositionDistance(a, b)` — L2 position distance.

## 4. Information-Matrix Validation (D-PG-05)

`ValidateInformationMatrix(info)` re-checks the flattened 6×6 (row-major, translation-then-rotation)
matrix is **symmetric, finite, and positive-definite/invertible**. `MakeIsotropicInfo6(pos_scale,
rot_scale)` builds a validated SPD diagonal matrix. This satisfies the 6c requirement that the info
matrix is **validated, not silently identity**.

## 5. Pose Graph Assembly (Trajectory → PoseGraph)

`AssemblePoseGraph(trajectory, nodes, options)` builds:

- `graph_nodes` — one `PoseGraphNode` per trajectory node (id/frame_id/timestamp only, D-PG-03).
- `graph_edges` — `N-1` odometry edges whose relative transform is the true pose difference
  `T_i{i+1} = Ti^{-1} * T{i+1}` between consecutive trajectory nodes, with a validated info matrix
  and confidence. The graph is internally consistent with the un-optimized ("acquisition")
  trajectory — the correct contract for an un-optimized graph (D-OPT-05: originals preserved).

## 6. Loop-Closure Pipeline (D-LC-01…D-LC-09)

- `DetectCandidates(trajectory, nodes, options)` — produces candidates between the newer source node
  and older target nodes, **excluding pairs below `minimum_temporal_separation_ns`** (D-LC-06/D-LC-08).
- `VerifyCandidate(candidate, nodes, options)` — backend-independent geometric verifier returning a
  `LoopClosure` (`accepted`/`rejected`) with inlier stats, `temporal_separation_ns`,
  `spatial_separation_m`, and confidence. Enforces the minimum-temporal-separation rule again and a
  **spatial-consistency** false-positive defense (D-LC-09).
- `BuildLoopClosureEdge(lc, nodes, edge_id, config_hash, ...)` — builds the canonical
  `loop_closure` `PoseGraphEdge` (source ref = `"loop_closure_verifier"`, edge `source` string,
  validated info matrix). Returns `nullopt` when the closure is rejected or the info matrix fails
  validation.

The loop-closure **measurement** for an exact revisitation is a near-identity relative pose
(`relative_position = (0,0,0)`, identity rotation): the revisited start frame observes the same
scene, so the measured pose of the current frame matches the origin frame. This is a *measurement*
input representing the revisit; the corrected poses are produced by GTSAM.

## 7. E2E Wiring (uses existing `optimize()`)

1. Build the drifted closed-square trajectory (deterministic, P0…P4 with P4 not returning to origin).
2. `AssemblePoseGraph` → canonical graph with odometry edges.
3. `DetectCandidates` → `VerifyCandidate` (accepted) → `BuildLoopClosureEdge`.
4. `RunOptimize` WITHOUT the loop edge, then WITH it (identical inputs otherwise, anchor on).
5. Compare drift metrics; persist outputs + provenance to the CAS store.

---

## 8. Normative references honored

- D-PG-04 edge `type` vocabulary: `loop_closure` used verbatim.
- D-PG-05 / D-PG-06 / D-PG-07 info matrix + confidence + provenance-on-edge.
- D-LC-04/D-LC-05/D-LC-06/D-LC-08/D-LC-09 verification semantics.
- D-OPT-03/D-OPT-05 original trajectory never modified; optimized poses in a separate document.
- D-PL-01 provenance DAG: `optimization_result → [trajectory_hash, ...]`, `optimized_trajectory →
  [optimization_result_hash]`.
