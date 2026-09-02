# P3-impl-7 Blocker Resolution — Optimized Trajectory → Reconstruction v2

**Status:** Architecture decision record
**Date:** 2026-09-02
**Author:** opencode
**Depends on:** P3-impl-7-implementation-mapping.md (§16.1, §16.3, §16.4)
**Scope:** Resolves three architectural blockers before P3-impl-7 implementation begins. No code, schema, build, or type changes.

---

## 1. Blockers addressed

| # | Blocker | Source | Severity |
|---|---------|--------|----------|
| 1 | Coordinate-frame declaration — how Trajectory and Reconstruction declare/resolve their frame relationship | Mapping §16.1 | **PRIMARY** — blocks seam design |
| 2 | Reconstruction lifecycle — DB API to supersede an existing reconstruction row | Mapping §16.3 | Required — blocks persistence |
| 3 | Point recomputation scope boundary — explicit Option A constraints | Mapping §16.4 | Scope definition — not blocking |

---

## 2. BLOCKER 1 — Coordinate Frame Declaration

### 2.1 The gap

`Trajectory.coordinate_frame` (`trajectory.h:56`) and `Reconstruction.coordinate_frame` (`reconstruction.h:122`) are **free-form string labels** (e.g. `"trajectory_0"`, `"reconstruction_0"`). The existing `FrameGraph` infrastructure operates on `FrameId` (UUID-backed, `frame_id.h:14`). There is no declared mechanism to map from the string labels to the `FrameGraph`, and neither entity declares which other frame it relates to.

The COLMAP adapter (`adapters/colmap/colmap_trajectory_adapter.h`) happens to produce a trajectory whose frame is identical to the reconstruction's frame (both derive from the same COLMAP model), but this relationship is an implicit side-effect of the adapter, not an architectural declaration.

### 2.2 What `coordinate_frame` means

- **On `Trajectory`** (`trajectory.h:56`): a human-readable label for the trajectory's local coordinate frame. The trajectory poses (`T_trajectory_camera`) are expressed relative to this frame. The label carries no identity semantics; two entities sharing a label are not necessarily in the same frame.

- **On `Reconstruction`** (`reconstruction.h:122`): a human-readable label for the reconstruction's local coordinate frame. The reconstruction camera poses (`T_reconstruction_camera`) and 3D points (`ReconPoint3D.xyz`) are expressed relative to this frame. Same label-carrying convention.

- **Both are labels, not identities.** The `coordinate_frame` string was designed as a descriptive annotation (D-TRJ-04, D-CRM-06), not as a `FrameId`-equivalent key. Two entities with different `coordinate_frame` strings may still be in the same frame (COLMAP case); two entities with the same string may not be (if the string is reused across unrelated reconstructions).

### 2.3 Decision: Caller-supplied alignment transform (direct SE3 parameter)

**The seam `ApplyOptimizedTrajectory` accepts `geometry::SE3 reconstruction_from_trajectory` as a direct parameter.** The caller (pipeline stage) is responsible for resolving this transform before invoking the seam.

**Rationale (in order of priority):**

1. **Minimal scope.** The seam is a pure function on canonical P2.5/P3 types. It does not import `FrameGraph`, `FrameId`, or `CoordinateFrame`. This preserves the adapter boundary (D-AB-01/D-AB-02) and keeps the seam unit-testable without constructing a `FrameGraph`.

2. **The transform is the only thing the seam needs.** The seam applies `T_reconstruction_trajectory · T_trajectory_camera` to produce `T_reconstruction_camera` (Mapping §4.6). Whether the caller resolved this via `FrameGraph::Transform()`, a caller-side lookup table, or a hardcoded identity is irrelevant to the seam.

3. **COLMAP case is identity.** When the trajectory was derived from the same COLMAP model as the source reconstruction, `T_reconstruction_trajectory = Identity`. The caller declares this explicitly (no implicit assumption inside the seam).

4. **No schema changes required.** The `coordinate_frame` string stays as a label on both `Trajectory` and `Reconstruction`. No new fields, no new type, no migration.

5. **FrameGraph is available for the caller.** The pipeline stage that orchestrates the feedback can build a `FrameGraph` from persisted `CoordinateFrame` nodes (future increment), or use a simple caller-side identity predicate for the first increment. The FrameGraph infrastructure is **not blocked** by this decision — it simply is not required inside the seam itself.

### 2.4 How the caller resolves the transform

**First increment (P3-impl-7):**

The pipeline stage applies a simple rule:

```
if (trajectory.coordinate_frame == reconstruction.coordinate_frame) {
    T_reconstruction_trajectory = SE3::Identity();
} else {
    // Resolve from caller-maintained mapping or FrameGraph
    // (see §2.5 for the deterministic-FrameId path)
}
```

This handles the COLMAP same-model case (identical strings → identity) and is extensible to future frame-graph resolution without changing the seam contract.

**Future increment (when FrameGraph is populated):**

A deterministic `FrameId` can be derived from any `coordinate_frame` string:

```cpp
FrameId FrameIdFromLabel(const std::string& label) {
    // Stable UUIDv5 derivation: namespace-spatial + label string.
    // Same label → same FrameId across runs, no lookup table needed.
    return FrameId(Uuid::V5(kSpatialNamespace, label));
}
```

The caller registers both `FrameIdFromLabel(trajectory.coordinate_frame)` and `FrameIdFromLabel(reconstruction.coordinate_frame)` as `CoordinateFrame` nodes in a `FrameGraph`, then calls `FrameGraph::Transform(recon_frame, traj_frame)` to obtain `SE3`. This is a **pipeline-stage concern**, not a seam concern.

### 2.5 What happens when frames differ

| Scenario | Transform | Behavior |
|----------|-----------|----------|
| Same `coordinate_frame` string (COLMAP same-model) | `Identity` | `T_reconstruction_camera = T_trajectory_camera` — raw write-back, numerically unmodified (Mapping §5.2, §4.6) |
| Different `coordinate_frame` strings, alignment available | Caller-resolved `SE3` | `T_reconstruction_camera = T_reconstruction_trajectory · T_trajectory_camera` — correct frame expression (Mapping §4.6) |
| Different `coordinate_frame` strings, **no** alignment available | **Error** | The seam must fail closed — it must not write raw `T_trajectory_camera` into a Reconstruction whose points are in a different frame (Mapping §5.4). The caller is responsible for detecting this condition before invoking the seam. |

### 2.6 Code-level implications of this decision

**Seam interface** (`reconstruction_feedback.h`, new file):

```cpp
struct ReconstructionFeedbackInput {
    Reconstruction source;
    Trajectory trajectory;
    std::vector<OptimizedPoseNode> optimized_nodes;
    geometry::SE3 reconstruction_from_trajectory;  // T_reconstruction_trajectory; caller resolves
    // ... provenance fields ...
};

ReconstructionFeedbackResult ApplyOptimizedTrajectory(
    const ReconstructionFeedbackInput& input);
```

The seam signature gains `reconstruction_from_trajectory` as a direct `geometry::SE3` parameter. No `FrameGraph`, `FrameId`, or `CoordinateFrame` types appear in the seam header.

**Files untouched by this decision:** `core/coordinates/*`, `core/reconstruction/reconstruction.h`, `core/trajectory/trajectory.h`, `schemas/json/reconstruction.schema.json`, `schemas/json/trajectory.schema.json`.

### 2.7 Normative statement

> **DECISION CF-1:** P3-impl-7 resolves the coordinate-frame relationship via a caller-supplied `geometry::SE3 reconstruction_from_trajectory` parameter on the seam. The `coordinate_frame` string on `Trajectory` and `Reconstruction` remains a descriptive label. FrameGraph-based resolution is a pipeline-stage concern, not a seam concern. The COLMAP same-model case declares `Identity` explicitly.

---

## 3. BLOCKER 2 — Reconstruction Lifecycle Update

### 3.1 Current DB state

`MetadataDb` (`metadata_db.h:283`) provides:

| Method | Purpose | Line |
|--------|---------|------|
| `AddReconstruction(ReconstructionRow)` | INSERT a new reconstruction row | `metadata_db.h:450` |
| `QueryLatestReconstructionByScene(scene_id)` | SELECT latest `status='succeeded'` row for a scene | `metadata_db.h:451` |
| `FindReconstructionsByScene(scene_id)` | SELECT all rows for a scene (all statuses) | `metadata_db.h:453` |

**Missing:** No UPDATE path for `ReconstructionRow.status`. The existing `ReconstructionRow.status` vocabulary (`"reconstructing" | "succeeded" | "failed" | "superseded"` at `metadata_db.h:190`) already includes `"superseded"`, but there is no method to transition a row to that status.

The `QueryLatestReconstructionByScene` SQL (`metadata_db.cpp:1869`) filters `WHERE status = 'succeeded'`, so superseding a row correctly removes it from "latest" resolution.

### 3.2 Required API addition

```cpp
// metadata_db.h — add after AddReconstruction/QueryLatest/FindReconstruction block

// Transitions a reconstruction row's status. Validates that the current status
// is one of {"reconstructing", "succeeded", "failed"} and the target status is
// one of {"superseded", "failed"}. Throws StorageError on invalid transition
// or missing row.
void SetReconstructionStatus(const Uuid& reconstruction_id,
                             const std::string& new_status);
```

**SQL implementation** (`metadata_db.cpp`):

```sql
UPDATE reconstructions SET status = ? WHERE reconstruction_id = ?
```

**Transaction semantics:** Single-statement UPDATE within the caller's existing transaction context. `MetadataDb` uses WAL mode (ADR-008); the UPDATE is atomic within the SQLite transaction. No new migration is required — the `reconstructions` table and its `status` column already exist (migration 0007).

**Allowed transitions:**

| From | To | Valid? |
|------|----|--------|
| `"reconstructing"` | `"succeeded"` | Yes |
| `"reconstructing"` | `"failed"` | Yes |
| `"reconstructing"` | `"superseded"` | Yes |
| `"succeeded"` | `"superseded"` | **Yes** — the P3-impl-7 use case |
| `"succeeded"` | `"failed"` | No |
| `"failed"` | `"succeeded"` | No |
| `"superseded"` | anything | No (terminal state) |

**Error conditions:**
- Row not found → `StorageError` with descriptive message
- Invalid transition (e.g. `"superseded"` → `"succeeded"`) → `StorageError`
- Read-only database → existing `kStorageReadOnly` error (consistent with `AddReconstruction`)

### 3.3 Does this belong in P3-impl-7 or a prerequisite increment?

**This is a prerequisite increment.** The status UPDATE is a small, bounded, standalone DB-layer addition that:
- Requires no schema migration (the column and vocabulary already exist)
- Is independently testable
- Is required by P3-impl-7 but is not specific to it (P3-impl-7c already flagged the same gap for trajectory/pose-graph status updates)

**Recommendation:** Implement `SetReconstructionStatus` as a standalone 1-file change (`metadata_db.h` + `metadata_db.cpp`) before P3-impl-7 implementation begins. It unblocks P3-impl-7 and is consistent with the existing `AddReconstruction`/`AddTrajectory` pattern.

### 3.4 Normative statement

> **DECISION LC-1:** P3-impl-7 requires a `SetReconstructionStatus(Uuid, string)` method on `MetadataDb`. This method is a prerequisite increment (standalone, no migration needed). Allowed transitions include `"succeeded" → "superseded"` for the supersede path. The method validates transitions and throws on invalid state changes.

---

## 4. BLOCKER 3 — Point Recomputation Scope Boundary

### 4.1 Option A constraints (explicit)

P3-impl-7 operates under **Option A — camera poses only** (Mapping §6):

| Element | Action | Rationale |
|---------|--------|-----------|
| `ReconImage.pose` | **Updated** from optimized trajectory | Core deliverable |
| `ReconPoint3D.xyz` | **Unchanged** — copied verbatim from source | Option A scope boundary |
| `ReconPoint3D.track` | **Unchanged** | No re-triangulation |
| `ReconCamera` | **Unchanged** | No calibration refinement |
| `ReconImage.detected` | **Preserved** (only matched images touched) | No re-localization |

**No re-triangulation. No bundle adjustment. No point-cloud transformation.** The 3D points remain in the original reconstruction's coordinate frame.

### 4.2 Frame-mixing analysis under Option A

When `T_reconstruction_trajectory = Identity` (COLMAP same-model):
- `ReconImage.pose` is updated from `T_trajectory_camera` (which equals `T_reconstruction_camera` in this case)
- `ReconPoint3D.xyz` is unchanged
- **No frame inconsistency:** both are in `reconstruction_0`

When `T_reconstruction_trajectory ≠ Identity` (different frames, alignment applied):
- `ReconImage.pose` is computed as `T_reconstruction_trajectory · T_trajectory_camera` — expressed in `reconstruction_0`
- `ReconPoint3D.xyz` is unchanged — already in `reconstruction_0`
- **No frame inconsistency:** the alignment transform only affects the camera pose write-back; points were never transformed. Both remain in the reconstruction frame.

**There is no frame-mixing risk under Option A.** The points are never touched, so they cannot end up in the wrong frame.

### 4.3 What changes if points are recomputed (future increment)

If a future increment (full BA or re-triangulation) recomputes 3D points from the optimized trajectory:

- New `ReconPoint3D.xyz` would be in the **optimized trajectory's frame** (the frame the BA operates in)
- If the trajectory frame differs from the reconstruction frame, a **one-time rigid transform** of all point coordinates would be needed: `p_reconstruction = T_reconstruction_trajectory · p_trajectory`
- This transform must be applied to every `ReconPoint3D.xyz` and to the `ReconPoint3D.track` image associations (since track observations reference images whose poses are now in the reconstruction frame)
- **This is out of scope for P3-impl-7** and must be flagged as a prerequisite for any point-recomputation increment

### 4.4 Normative statement

> **DECISION SC-1:** P3-impl-7 operates under Option A scope boundary. Camera poses are updated; 3D points, cameras, and detection flags are unchanged. No re-triangulation, no bundle adjustment, no point-cloud transformation. When coordinate frames differ, the alignment transform applies only to camera pose write-back; points remain in the original reconstruction frame. Point recomputation is explicitly deferred and flagged for future increments.

---

## 5. Exact code changes required

### Files to create (P3-impl-7 implementation phase)

| File | Purpose |
|------|---------|
| `core/trajectory/reconstruction_feedback.h` | Seam header: `ReconstructionFeedbackInput`, `ReconstructionFeedbackResult`, `ApplyOptimizedTrajectory`, `ValidateOptimizedReconstruction` |
| `core/trajectory/reconstruction_feedback.cpp` | Seam implementation: join by `frame_id`, apply `T_reconstruction_trajectory · T_trajectory_camera`, produce new `Reconstruction`, determinism |
| `engine/pipeline/<new>_stage.h` | Pipeline orchestration stage header |
| `engine/pipeline/<new>_stage.cpp` | Orchestration: read inputs, resolve frame alignment, call seam, persist via MetadataDb |
| `tests/unit/test_reconstruction_feedback.cpp` | Unit tests: all 17 tests from Mapping §14 |

### Files to modify

| File | Change | Location |
|------|--------|----------|
| `core/storage/metadata_db.h` | Add `SetReconstructionStatus(Uuid, string)` declaration | After `FindReconstructionsByScene` block (~line 454) |
| `core/storage/metadata_db.cpp` | Add `SetReconstructionStatus` definition (UPDATE SQL) | After `FindReconstructionsByScene` implementation (~line 1930) |

### Files that must remain untouched

| File | Reason |
|------|--------|
| `core/reconstruction/reconstruction.h` | P2.5 canonical types; no change warranted |
| `core/trajectory/trajectory.h` | P3 canonical types; no change warranted |
| `core/trajectory/optimization.h` | Optimization types; no change warranted |
| `core/trajectory/pose_graph.h` | Pose graph types; no change warranted |
| `core/scene/frame.h` | Bridge entity; no change warranted |
| `core/coordinates/frame_id.h` | FrameId UUID type; no change warranted |
| `core/coordinates/coordinate_frame.h` | CoordinateFrame type; no change warranted |
| `core/coordinates/frame_graph.h` | FrameGraph type; no change warranted |
| `core/geometry/se3.h` | SE3 type; no change warranted |
| `core/geometry/quaternion.h` | Quaternion type; no change warranted |
| `schemas/json/reconstruction.schema.json` | P2.5 schema; no change warranted |
| `schemas/json/trajectory.schema.json` | P3 schema; no change warranted |
| `schemas/json/optimization-result.schema.json` | P3 schema; no change warranted |
| `schemas/json/pose-graph.schema.json` | P3 schema; no change warranted |
| `adapters/gtsam/*` | GTSAM adapter boundary (D-AB-01/D-AB-02); no change warranted |
| `adapters/colmap/*` | COLMAP adapter; no change warranted |
| `core/artifacts/*` | CAS store; no change warranted |

---

## 6. Updated P3-impl-7 implementation scope

### In scope

1. **Seam function** `ApplyOptimizedTrajectory` — pure function on canonical types, accepts explicit `T_reconstruction_trajectory`, produces new `Reconstruction` (Option A)
2. **Join logic** — `OptimizedPoseNode.frame_id` ↔ `ReconImage.frame_id` with four cases (Mapping §3.2)
3. **Transform application** — `T_reconstruction_trajectory · T_trajectory_camera` → `T_reconstruction_camera` (Mapping §4.6)
4. **Revision semantics** — new `reconstruction_id`, new CAS payload, source row → `"superseded"` (Mapping §7)
5. **Provenance lineage** — `backend.name = "spatial_optimizer"`, acyclic chain (Mapping §8)
6. **Determinism** — identical inputs → identical CAS hash (Mapping §11)
7. **Schema validation** — output validates against `reconstruction.schema.json` v2
8. **DB persistence** — insert new row, supersede old row via `SetReconstructionStatus`
9. **Pipeline stage** — orchestration: read inputs, resolve alignment, call seam, persist
10. **Full test suite** — 17 tests from Mapping §14

### Out of scope (explicitly deferred)

1. 3D-point recomputation / re-triangulation / bundle adjustment (future)
2. Point-cloud rigid transform for cross-frame alignment (future, prerequisite for point recomputation)
3. FrameGraph-based frame registration from `coordinate_frame` strings (pipeline-stage concern, future)
4. `FrameIdFromLabel` deterministic UUID derivation (future helper)
5. ORB-SLAM3 / KISS-ICP adapters (P3-impl-4/9)
6. Loop-closure implementation (P3-impl-7a/7b/7c — separate thread)
7. Multi-session alignment, map merge, uncertainty propagation to points

---

## 7. Updated test requirements

### Per-decision test needs

**Coordinate-frame decision (CF-1):**
- `CoordinateFrameIdentity` — identical strings → identity alignment → raw pose write-back (test #6 from Mapping §14)
- `CoordinateFrameAligned` — different strings + caller-supplied SE3 → correct composition (test #7)
- `CoordinateFrameMismatch` — different strings + no alignment → seam rejects/fails closed (test #5)
- `NoDoubleInversion` — `T_rt * T_tc` (not inverted) — golden numeric check (test #8)

**Lifecycle decision (LC-1):**
- `SetReconstructionStatus_SucceedToSuperseded` — valid transition succeeds
- `SetReconstructionStatus_InvalidTransition` — `"superseded"` → `"succeeded"` throws
- `SetReconstructionStatus_RowNotFound` — nonexistent UUID throws
- `SetReconstructionStatus_ReadOnly` — read-only DB throws (existing pattern)
- `RevisionSemantics` — new `reconstruction_id`, source row status transitions to `"superseded"` (test #15)

**Scope boundary decision (SC-1):**
- `OptionAPointsUnchanged` — `ReconPoint3D.xyz`, `track`, `ReconCamera`, `detected` byte-identical to source; only matching `ReconImage.pose` changes (test #14)

**All existing Mapping §14 tests (1–17) remain required.** No tests are removed by these decisions; the decisions add clarity to what each test verifies.

### New tests from lifecycle decision

| # | Test | What it verifies |
|---|------|------------------|
| 18 | `SetReconstructionStatus_ValidTransition` | `"succeeded"` → `"superseded"` succeeds; row reflects new status |
| 19 | `SetReconstructionStatus_TerminalRejects` | `"superseded"` → `"succeeded"` throws StorageError |
| 20 | `SetReconstructionStatus_MissingRow` | Nonexistent reconstruction_id throws StorageError |

---

## 8. Remaining architectural blockers

After resolving the three blockers above, the following items remain:

| # | Item | Severity | Blocks |
|---|------|----------|--------|
| 1 | **`SetReconstructionStatus` implementation** (prerequisite increment) | Required | P3-impl-7 persistence |
| 2 | **Pipeline stage naming/orchestration** — the engine stage that reads inputs, resolves alignment, calls the seam, and persists results | Design | P3-impl-7 orchestration (not the seam itself) |
| 3 | **Deterministic timestamp policy** — what `created_at_ns` and provenance timing values to use for the new CAS document (Mapping §11.3) | Design | CAS determinism |
| 4 | **`backend.name = "spatial_optimizer"` vocabulary ratification** — new entry not in existing P2.5 examples (Mapping §8.2) | Minor | Provenance clarity |

**None of these are architectural blockers for the seam design or scope.** They are implementation-phase design decisions that can be resolved during coding without changing the architectural decisions in this document.

---

*This blocker-resolution report was produced by opencode on 2026-09-02. No code, schema, dependency, or build changes were made.*
