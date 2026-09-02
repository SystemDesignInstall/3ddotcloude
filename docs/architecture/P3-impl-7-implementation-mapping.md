# P3-impl-7 Implementation Mapping — Optimized Trajectory → Reconstruction v2 (Pose-Only Feedback)

**Status:** Pre-code architectural gate — mapping only
**Date:** 2026-09-02
**Author:** opencode
**Depends on:** P2.5 ✅, P3-impl-1 ✅, P3-impl-3 ✅, P3-impl-6 (GTSAM adapter) ✅
**Scope:** Mapping document only — no code, schema, dependency, or build changes

> **Note on numbering.** Within this repository the `P3-impl-7a/7b/7c` sequence currently denotes the
> *loop-closure* work (visual candidate generation, geometric verification, metric loop edges). This
> document is a *separate* increment with the same short-hand `P3-impl-7`: the **feedback of an
> optimized (post-GTSAM) trajectory into a new Reconstruction revision**. It is the mapping for the
> "Reconstruction v2 Integration" design section (D-RI-01…D-RI-04) of
> `docs/architecture/P3-trajectory-pose-graph-loop-closure.md` §12. The two meanings of "impl-7"
> must not be conflated.

---

## 1. Purpose and scope

This document maps how an **optimized trajectory** (a `Trajectory` + `OptimizationResult` produced by
the GTSAM adapter, P3-impl-6) feeds back into a **new P2.5 Reconstruction** document. It covers the
exact join semantics, the pose-direction and coordinate-frame conventions at every boundary, revision
semantics, provenance lineage, and the concrete C++ API surface required — all without changing any
code.

**Recommended first-increment decision (this document's sole integration scope):**

> **Option A — camera poses only.** `ReconImage.pose` is updated from the optimized trajectory.
> `ReconPoint3D.xyz`, `ReconCamera`, and detection flags are preserved from the source
> Reconstruction. 3D points stay in the source Reconstruction's coordinate frame.

The rationale, correctness argument, and the coordinate-frame caveat for Option A are in §7.

---

## 2. Canonical types referenced (exact locations)

| Type | Header / schema | Purpose |
|---|---|---|
| `Reconstruction` | `core/reconstruction/reconstruction.h:118` | Root reconstruction entity |
| `ReconImage` | `core/reconstruction/reconstruction.h:49` | Image + `pose` + `detected` + `frame_id` |
| `ReconPose` | `core/reconstruction/reconstruction.h:24` | `rotation_xyzw`, `translation_xyz` |
| `ReconCamera` | `core/reconstruction/reconstruction.h:32` | Intrinsics |
| `ReconPoint3D` | `core/reconstruction/reconstruction.h:61` | `xyz`, `track` |
| `ReconstructionProvenance` | `core/reconstruction/reconstruction.h:87` | Backend, hashes, timing |
| `TrajectoryPoseNode` | `core/trajectory/trajectory.h:38` | Pose node (`frame_id`, pose) |
| `Trajectory` | `core/trajectory/trajectory.h:51` | Root (coordinate_frame, kind, status, provenance) |
| `OptimizedPoseNode` | `core/trajectory/optimization.h:42` | Corrected pose (`frame_id`, pose) |
| `OptimizationResult` | `core/trajectory/optimization.h:55` | Optimization metadata |
| `PoseGraph` / `PoseGraphNode` / `PoseGraphEdge` | `core/trajectory/pose_graph.h:63/25/34` | Constraints |
| `Frame` | `core/scene/frame.h:15` | Bridge entity (`frame_id`, `session_id`, `timestamp_ns`, `sequence_index`) |
| `OptimizationInput`/`OptimizationOutput`/`optimize()` | `adapters/gtsam/gtsam_optimizer_adapter.h:45/56/64` | GTSAM boundary |
| `ReconstructionRow` | `core/storage/metadata_db.h:186` | DB metadata row |
| `Reconstruction` (JSON) | `schemas/json/reconstruction.schema.json` | CAS document schema (v2) |
| `Trajectory` (JSON payload) | `schemas/json/trajectory.schema.json` | Trajectory payload (nodes) |
| `OptimizationResult` (JSON) | `schemas/json/optimization-result.schema.json` | Result payload |
| `PoseGraph` (JSON) | `schemas/json/pose-graph.schema.json` | Pose graph payload |
| `SE3` | `core/geometry/se3.h:13` | Rigid transform (`p' = R·p + t`) |
| `FrameGraph` / `CoordinateFrame` | `core/coordinates/frame_graph.h:18` / `core/coordinates/coordinate_frame.h:15` | Frame topology + alignment |

---

## 3. Join semantics — `TrajectoryPoseNode` ↔ `ReconImage` ↔ `Frame`

### 3.1 The bridge key

All three entities reference a common bridge identity:

- `Frame.frame_id` (`core/scene/frame.h:16`) — the UDPv5, deterministic, immutable frame identity.
- `TrajectoryPoseNode.frame_id` (`core/trajectory/trajectory.h:39`) — a UUID string; D-TRJ-10 bridge reference; **must** equal `Frame.frame_id` when non-empty.
- `ReconImage.frame_id` (`core/reconstruction/reconstruction.h:52`) — a UUID string; D-CRM-18 bridge reference; **optional** (may be empty).
- `OptimizedPoseNode.frame_id` (`core/trajectory/optimization.h:43`) — carried through from the source `PoseGraphNode.frame_id` (P3-impl-6 result extraction), hence equals the original trajectory's `frame_id`.

The join for the pose-feedback mapping is therefore **by `frame_id` string equality**:

```
OptimizedPoseNode.frame_id  ==  TrajectoryPoseNode.frame_id  ==  ReconImage.frame_id
                                      (also == Frame.frame_id when Frame resolved)
```

The `OptimizedPoseNode` vector (ordered by `sequence_index`) is the authoritative corrected-pose
source (D-OPT-03/D-OPT-04); the original `TrajectoryPoseNode` vector supplies the original poses for
unoptimized / unmatched handling and identity cross-checks.

### 3.2 The four cases

The mapping must handle each join outcome deterministically:

**(a) Both trajectory node and ReconImage present, frame_id non-empty and matching.**
Apply the optimized pose to `ReconImage.pose` (with the frame-transform from §5). This is the normal
path for a detected image that had a pose.

**(b) Trajectory node present, no matching ReconImage.**
The trajectory may legitimately contain more frames than the reconstruction has registered images
(e.g. a frame localized by odometry but rejected by the reconstruction). **Skip** — no
`ReconImage` is created or modified for this node (§9).

**(c) ReconImage present, no matching trajectory node.**
This image was never in the optimized trajectory (e.g. COLMAP registered an image whose frame the
trajectory backend omitted). **Keep the original `ReconImage.pose`** unchanged (§9).

**(d) Missing `frame_id` (empty on either side).**
An empty `frame_id` cannot be joined. Handle gracefully per the rules in §10 (skip the trajectory
node / keep the original ReconImage pose), never fail hard.

### 3.3 Join index construction (deterministic)

Build an index `frame_id → OptimizedPoseNode` and `frame_id → ReconImage` from the *ordered* input
vectors. Iterate the `OptimizedPoseNode` vector **in ascending `sequence_index` order** (§11), which
is also the iteration order guaranteed by `optimization_result.schema.json` ("ordered by
sequence_index") — so the output `ReconImage` list is produced in a deterministic order.

---

## 4. Pose direction and convention at every boundary

### 4.1 The platform-wide naming rule (ADR-007 / ADR-018)

Per `docs/architecture/coordinate-systems.md` §4: a transform named `A_from_B` (read "B expressed in
A") maps a point from frame B into frame A:

```
p_A = R_AfromB · p_B + t_AfromB
```

`SE3` (`core/geometry/se3.h:3`, `:46`) uses exactly this `p' = R·p + t` convention. There is no
ambiguity in the platform's own notation.

### 4.2 `T_reconstruction_camera` (P2.5)

`ReconPose` (`core/reconstruction/reconstruction.h:24`) carries `T_reconstruction_camera`
(`reconstruction.h:10`; schema `reconstruction.schema.json:72` — "Maps points from camera frame to
the reconstruction's local frame"):

```
p_reconstruction = R_rc · p_camera + t_rc
```

This is **reconstruction-from-camera** (camera → reconstruction; camera is the "body", the
reconstruction frame is the "world/local" frame). Quaternion is scalar-last `(x,y,z,w)` per ADR-007
(`reconstruction.schema.json:81`).

### 4.3 `T_trajectory_camera` (P3)

`TrajectoryPoseNode` carries `T_trajectory_camera` (`trajectory.h:10,35-36,42-43`; schema
`trajectory.schema.json:28` — "transform mapping a point from the camera/body frame into the
trajectory's local coordinate frame"):

```
p_trajectory = R_tc · p_camera + t_tc
```

This is **trajectory-from-camera** (world-from-body **in the trajectory's local frame**), the
P3 analogue of `T_reconstruction_camera` (D-CF-02). Quaternion scalar-last.

### 4.4 The GTSAM-optimized pose

`OptimizedPoseNode.position_xyz/rotation_xyzw` (`core/trajectory/optimization.h:42-51`; schema
`optimization-result.schema.json:142,149`) are **still `T_trajectory_camera`** — identical
convention to the pre-optimization nodes, only the numerical values are corrected (D-OPT-03,
`optimization.h:9`). The P3-impl-6 mapping proved GTSAM `Pose3` is world-from-body in the same sense;
no SE(3) inversion is introduced by the optimizer.

### 4.5 Is an SE(3) inversion needed when writing back?

**Only if the trajectory frame and reconstruction frame differ** (see §5.2). The corrected pose as
produced by GTSAM is `T_trajectory_camera`. The Reconstruction requires `T_reconstruction_camera`.
Because both are world-from-body in their *respective local* frames, the transform between them is a
pure frame-to-frame rigid transform `T_reconstruction_trajectory` (D-CF-05):

```
T_reconstruction_camera  =  T_reconstruction_trajectory · T_trajectory_camera
```

`T_trajectory_camera` is **not** inverted in this chain — it is right-multiplied by the alignment.
An inversion would be a *single* `SE3::Inverse()` (`se3.h:40`) only in the degenerate reasoning where
someone conflates the frames; under the derivation above no inversion of either pose occurs.

### 4.6 The transform chain (explicit)

```
T_trajectory_camera          (corrected pose, from OptimizationResult)
      │
      │ right-compose with the frame alignment (D-CF-05)
      ▼
T_reconstruction_trajectory · T_trajectory_camera
      │
      ▼
T_reconstruction_camera      (written to ReconImage.pose)
```

Equivalently, treating each pose as `SE3`:

```cpp
// Pseudo-C++ (mapping only; not an API proposal)
geometry::SE3 T_tc = MakeCameraPose(node.position_xyz, node.rotation_xyzw);      // corrected
geometry::SE3 T_rt = /* from FrameGraph alignment, or identity (§5.2/§5.4) */;
geometry::SE3 T_rc = T_rt * T_tc;                                                // to write
```

When `T_reconstruction_trajectory == Identity` (proven/assumed same frame, §5.2), `T_rc == T_tc` and
the values are written back as-is — no rotation/translation mutation.

---

## 5. Coordinate-frame compatibility

### 5.1 The two named frames

- `Trajectory.coordinate_frame` (`core/trajectory/trajectory.h:56`, D-TRJ-04) — e.g. `"trajectory_0"`. Its origin is arbitrary (often first pose or odometry origin) and need not equal the reconstruction origin.
- `Reconstruction.coordinate_frame` (`core/reconstruction/reconstruction.h:122`, D-CRM-06) — e.g. `"reconstruction_0"`, assigned by the reconstruction adapter (not `project_world` unless aligned).

Both are **local frames of their owning analysis result**; neither is `project_world` unless
explicitly aligned (D-CF-03/D-CF-04).

### 5.2 When are they the same vs. different?

**Same frame — the common, initially-planned case (COLMAP-derived trajectory).**
The COLMAP trajectory adapter (`adapters/colmap/colmap_trajectory_adapter.h:16-20`) computes
`T_trajectory_camera = InvertColmapPose()` from COLMAP's camera-to-world **in COLMAP's reconstruction
frame**. If the same COLMAP run produced both the source Reconstruction and the Trajectory, the
trajectory frame **is** the reconstruction frame — they describe the same origin and axes (up to the
frame *label*):

```
T_trajectory_camera  =  T_reconstruction_camera      (for the same COLMAP region, same run)
T_reconstruction_trajectory = Identity
```

Here the two `coordinate_frame` strings will typically differ (`"trajectory_0"` vs
`"reconstruction_0"`) as *labels* only, **not** as different frames.

**Different frames — the general case.**
A trajectory may be produced by an independent backend (ORB-SLAM, KISS-ICP — future), aligned to a
different origin, or reconstructed from a different session. Then `T_reconstruction_trajectory` is a
genuine rigid transform and **must** be supplied from the FrameGraph.

### 5.3 The required transform `T_reconstruction_trajectory`

The authoritative place such an alignment lives is the **FrameGraph** as an edge between the
`CoordinateFrame` nodes `reconstruction_0` (child) and `trajectory_0` (child) under a common root —
via `CoordinateFrame.parent_from_child` (`core/coordinates/coordinate_frame.h:19`) and
`FrameGraph::Transform(from, to)` (`core/coordinates/frame_graph.h:39`). The edge encodes
`T_reconstruction_trajectory` (a point in the trajectory frame, expressed in the reconstruction
frame).

```
T_reconstruction_trajectory  =  Transform(reconstruction_0, trajectory_0)
```

If the frames are proven identical, `Transform` returns `SE3::Identity()` (`frame_graph.h:37`).

### 5.4 What happens when coordinate frames don't match

- **If an alignment (`T_reconstruction_trajectory`) is available** (from the FrameGraph or a
  caller-provided rigid transform): apply it per §4.6 and write the transformed pose.
- **If frames are unknown/proven different and no alignment is available**: the mapping **must
  fail closed** — it must not write raw `T_trajectory_camera` values into a Reconstruction whose
  poses would then contradict `ReconPoint3D.xyz`. This is a hard, detectable error condition
  (§14 test `CoordinateFrameMismatch`).
- **Identity provable by origin**: when the trajectory derives from the *same* reconstruction
  region (COLMAP case, §5.2), the transform is identity and raw write-back is correct.

**Unresolved decision / potential blocker (stated explicitly — see §16):** the source
`Trajectory` and `OptimizationResult` payloads do **not** carry an explicit declaration of the
relationship between the trajectory frame and *a specific* Reconstruction's frame. `coordinate_frame`
is a free-form string label (D-TRJ-04 "follows the same naming convention"), and the COLMAP trajectory
adapter infers the same-frame property from having inverted the same COLMAP model. For a robust
implementation the mapping should either (1) require the caller to state `T_reconstruction_trajectory`
explicitly (identity is acceptable), or (2) introduce a lightweight provenance/declaration that the
trajectory was derived from a specific `reconstruction_id` whose frame it shares. **Recommendation:
(1) — require an explicit alignment transform (default identity), with the COLMAP-same-model case
declaring identity.** See §16.2.

---

## 6. What exactly gets updated (Option A)

For the first integration increment:

| Element | Action | Detail |
|---|---|---|
| `ReconImage.pose` | **Updated** | For every `ReconImage` with a non-empty matching `frame_id` whose pose was optimized (§3.2a), write `T_reconstruction_camera` computed in §4.6 (raw `T_trajectory_camera` when frames coincide). |
| `ReconImage.detected` | **Preserved** (only updated images touched) | `detected=false` images keep as-is (§9/§10); `detected=true` images that get a pose keep `detected=true`. The mapping applies the optimizer pose **only** to images it actually updated; it never flips `detected` to true for a previously-false image. |
| `ReconCamera` | **Unchanged** | Intrinsics/distortion are copied verbatim; no calibration refinement in this increment. |
| `ReconPoint3D` | **Unchanged** | `xyz` and `track` copied verbatim; 3D points **stay in the original reconstruction frame** (Option A). |
| `Reconstruction.coordinate_frame` | **Unchanged** | The output document is a new revision (§7) but keeps the **same** `coordinate_frame` string as the source (the poses written back are expressed in that frame, §5). |
| `Reconstruction.reconstruction_id` | **New UUID** | New instance id (UUIDv4, D-CRM-07); revision identity (§7). |
| `Reconstruction.status` | **`"succeeded"`** | The finalized CAS payload is `"succeeded"` (D-CRM-19, `reconstruction.schema.json:40-41`); lifecycle transitions live in the DB row (§7.4). |
| `Reconstruction.provenance` | **New** | Chains to the optimization (see §8). |
| `created_at_ns` | **New, deterministic** | See §11 (no wall-clock in CAS content). |

Everything not listed (session ids, scene id, cameras, points, image names, `image_id`/`camera_id`
integers) is copied unchanged from the source Reconstruction so that the output document is a valid
`reconstruction.schema.json` v2 document.

---

## 7. Reconstruction revision / immutability semantics

### 7.1 CAS artifacts are immutable

All payloads (`ArtifactStore`, `core/artifacts/artifact_store.h`, ADR-010) are content-addressed and
immutable (`artifact_store.h:6-7`). The original Reconstruction document, trajectory, pose graph, and
optimization result are **never modified**.

### 7.2 The optimized Reconstruction is a NEW document

Per D-RI-01/D-RI-04 and D-CRM-19, an optimized trajectory feeding back produces a **new**
`Reconstruction` with a **new `reconstruction_id`** (UUIDv4, instance identity — D-CRM-07:
"instance-scoped UUIDv4 identity ... not content-derived"). It is a fresh CAS payload with a fresh
content hash; the original document's bytes and hash are unchanged.

### 7.3 Status of the new document

- CAS document `status = "succeeded"` (`reconstruction.schema.json:40-41`; D-CRM-19: for a finalized
  payload this should be `"succeeded"`).
- DB `ReconstructionRow.status = "succeeded"` (`metadata_db.h:186-193`).

### 7.4 Status of the original Reconstruction

**Decision (this mapping): the original "source" Reconstruction's status becomes `"superseded"`.**

- The design (D-RI-04, `P3-trajectory-pose-graph-loop-closure.md:650`) is explicit: "The previous
  reconstruction's status becomes `"superseded"`."
- Mechanism: lifecycle belongs to the DB `ReconstructionRow` (`reconstruction.schema.json:5` —
  "Lifecycle state ... belongs to the DB ReconstructionRow, NOT this document"). So the update is a
  **DB row status update** on the prior `ReconstructionRow` from `"succeeded"` → `"superseded"`
  (`metadata_db.h:186-193`). The original CAS document's `status` field remains whatever it was
  (a finalized payload holds `"succeeded"` in the document; it is the row that flips).
- `QueryLatestReconstructionByScene` / `FindReconstructionsByScene` (`metadata_db.h:451-454`) resolve
  the active reconstruction from the row status, so superseding correctly repoints the "active"
  reconstruction.

> **Decision rationale.** The alternative — leaving the original `"succeeded"` — would make
> `QueryLatest...` ambiguous when multiple `"succeeded"` rows exist for a scene (the latest by
> `created_at_ns` would silently win, but with two `succeeded` rows there is no explicit "active"
> marker). Following D-RI-04 verbatim keeps one active reconstruction per scene. This matches the
> existing lifecycle vocabulary (`"reconstructing" | "succeeded" | "failed" | "superseded"` in
> `reconstruction.h:123`).

### 7.5 DB persistence

A new `ReconstructionRow` (`metadata_db.h:186`) is inserted via
`MetadataDb::AddReconstruction(...)` (`metadata_db.h:450`), storing the serialized optimized document
and metadata. The prior row's status is updated to `"superseded"` (this requires an `UPDATE` path
that — like the trajectory/pose-graph status UPDATE noted in P3-impl-7c §10 — does not yet exist and
must be added; see §16.3).

---

## 8. Provenance lineage

### 8.1 The exact chain

```
Reconstruction v2 (optimized)                         ← NEW CAS document
  ← OptimizationResult (result_id, D-OPT-01)          ← OptimizationResultRow + optimization-result.schema.json
  ← PoseGraph (graph_id, D-PG-01)                     ← PoseGraphRow + pose-graph.schema.json
  ← Trajectory (trajectory_id, D-TRJ-01)              ← TrajectoryRow + trajectory.schema.json
  ← Reconstruction v1 (original)                      ← joined via frame_id → Frame (D-CRM-18 / D-TRJ-10)
  ← COLMAP backend                                    ← original reconstruction provenance (backend.name="colmap")
```

The optimized Reconstruction's `ReconstructionProvenance` (`reconstruction.h:87`, schema
`reconstruction.schema.json:270`) carries:

- `backend.name` — **decision (this mapping): `"spatial_optimizer"`** (see §8.2);
- `backend.version` — the spatial optimizer / integration adapter version;
- `backend.adapter_version` — the integration adapter version;
- `input_artifact_hashes` — **includes the `OptimizationResult` CAS hash** (and the optimized
  trajectory payload / pose-node CAS hash where such a payload exists — see §8.4), plus the source
  Reconstruction hash and Trajectory/PoseGraph hashes;
- `engine_version` / `engine_commit` / `git_commit` — build identities;
- `started_at_ns` / `finished_at_ns` / `duration_ns` — deterministic timing (§11);
- `backend_specific_json` — e.g. `{"optimizer":"gtsam","optimization_result_id":"<uuid>",
  "coordinate_frame_alignement":"identity|frame_graph","option":"A"}`;
- `configuration_hash` — SHA-256 of the effective integration configuration (including the
  frame-alignment identity predicate).

### 8.2 `backend.name` — "spatial_optimizer" vs "gtsam"

**Decision (this mapping): `backend.name = "spatial_optimizer"`.**

Rationale: GTSAM is a downstream *optimizer*, produced and consumed behind the adapter boundary
(P3-impl-6) and must not leak as the "reconstruction backend." The entity that synthesizes a new
Reconstruction from an optimized trajectory is the trajectory→reconstruction integration layer (the
`ApplyOptimizedTrajectory` transform/function of §12), which is backend-independent. `"spatial_optimizer"`
names that integration layer, consistent with how `ReconstructionProvenance.backend_specific_json`
can still record the concrete `"gtsam"` detail. (The alternative — `"gtsam"` — would mislabel a
purely synthetic/integrative step as a reconstruction backend and leak an optimizer name into the
P2.5 provenance vocabulary. The COLMAP example at `reconstruction.schema.json:283` shows the intent:
the *backend that produced the reconstruction* is named.)

### 8.3 No circular references

The provenance chain is a strict **DAG**: each artifact references only earlier inputs via
`input_artifact_hashes` and never references its own output. The optimized Reconstruction is new; it
is not referenced by any of its own inputs (trajectory/pose-graph/optimization-result/original
reconstruction do not point forward to it). `input_artifact_hashes` form an acyclic graph (D-PL-01,
`P3-...-loop-closure.md:817`). Terminated: the original Reconstruction's provenance stops at the COLMAP
backend; no path cycles back. A circularity is impossible because no node writes itself into a
descendant's input set.

### 8.4 Where the optimization `OptimizationResult` live

`OptimizationResult` (metadata) is stored in `OptimizationResultRow`
(`core/storage/metadata_db.h:260`), and its full document (including `nodes`) validates against
`optimization-result.schema.json`. The optimized pose nodes are in that document's `nodes`
(`optimization-result.schema.json:61-65`). The `OptimizationResult` CAS hash is therefore the
canonical hash of the optimized poses and is the primary input hash referenced by the optimized
Reconstruction's provenance.

---

## 9. Handling unmatched nodes (trajectory vs reconstruction asymmetry)

- **Trajectory node with no matching `ReconImage`** → **skip** that optimized node; do not invent a
  `ReconImage`. (The trajectory may legitimately carry more frames than the reconstruction has
  registered images — e.g. odometry-localized-but-reconstruction-rejected frames.)
- **`ReconImage` with no matching trajectory node** → **keep the original `ReconImage.pose`**
  unchanged. (e.g. an image registered by COLMAP whose frame the trajectory backend omitted.)
- **`ReconImage` with `detected=false`** → **keep as-is** (copy the image record through unchanged);
  never attempt to optimize or re-localize it.
- All four cases are covered by join semantics §3.2 and by tests (§14).

---

## 10. Missing `frame_id` handling

`ReconImage.frame_id` is optional (schema `reconstruction.schema.json:172-176`: "Optional UUID");
`TrajectoryPoseNode.frame_id` is required by schema but may be empty in malformed/legacy data.

- **`OptimizedPoseNode.frame_id`/`TrajectoryPoseNode.frame_id` empty** → cannot join → **skip** the
  node (no `ReconImage` updated for it).
- **`ReconImage.frame_id` empty** → cannot join → **keep the original pose** for that image.
- **Log and continue.** A warning with the node/image ordinal (not raw GUID) is emitted; processing
  does not abort. This is a graceful, deterministic degradation — never a hard failure.

---

## 11. Deterministic identity and output

### 11.1 Same inputs → same output CAS hash

The optimized Reconstruction CAS document is computed from: the source Reconstruction + the
`OptimizationResult` (optimized nodes) + the frame-alignment predicate. Given identical inputs and a
deterministic serialization (canonical field order matching `reconstruction.schema.json` /
`ToJsonString`-style canonicalization), **the same inputs yield byte-identical output and therefore
the same content SHA-256 (CAS hash)**. (The *instance* `reconstruction_id` UUID is not
content-derived and may differ between runs — D-DI-01 — but the CAS bytes are identical and dedupe.)

### 11.2 Node processing order

The `ReconImage` output list is produced by iterating the `OptimizedPoseNode` vector **in ascending
`sequence_index`** (`optimization.h:45`, D-TRJ-07) — the order guaranteed by
`optimization-result.schema.json:61-64`. Images not updated keep their source order, re-merged so the
output `images[]` array is stable and reproducible.

### 11.3 No wall-clock in CAS payload content

CAS content must not embed a wall-clock `created_at_ns`. Per ADR-010/immutability, `created_at_ns`
is set deterministically (e.g. carried from the `OptimizationResult.created_at_ns` or the source
Reconstruction's deterministic timestamp), so identical inputs produce identical payload bytes. The
provenance timing fields (`started_at_ns`/`finished_at_ns`/`duration_ns`) follow the same rule for
the purposes of CAS determinism — use deterministic derivable values, or keep them out of the
content-hashed document (move per-run timing to the DB row).

---

## 12. Exact C++ API changes required (mapping — not authorized)

All changes below are **proposed** for the implementation phase; none are made here. Where a type
already exists it is referenced verbatim; new symbols are marked **(new)**.

### 12.1 New integration function (the core seam)

**`(new)` `core/trajectory/reconstruction_feedback.h`** (header-only, canonical, backend-free —
consistent with `pose_graph_helpers.h` style):

```
namespace spatial::core {

struct ReconstructionFeedbackInput {
  Reconstruction source;                                  // original v1 (immutable read)
  Trajectory trajectory;                                  // for coordinate_frame + provenance
  std::vector<TrajectoryPoseNode> trajectory_nodes;       // original nodes (cross-check)
  OptimizationResult optimization_result;                 // result_id, status, provenance
  std::vector<OptimizedPoseNode> optimized_nodes;         // corrected T_trajectory_camera
  geometry::SE3 reconstruction_from_trajectory;           // T_reconstruction_trajectory
                                                          // (default Identity; caller must set)
  std::vector<Frame> frames;                              // (optional) bridge resolution
};

struct ReconstructionFeedbackResult {
  Reconstruction reconstruction;                          // NEW revision (v2)
};

// Applies an optimized trajectory to a source reconstruction -> new revision (Option A:
// camera poses only; points/cameras/detected unchanged). Deterministic, log-continue on
// unmatched/missing frame_id. Throws on coordinate-frame mismatch when no alignment
// (or returns a typed error) and on empty optimization result.
ReconstructionFeedbackResult ApplyOptimizedTrajectory(
    const ReconstructionFeedbackInput& input);

// Deterministic schema+consistency validation of the produced document.
bool ValidateOptimizedReconstruction(const Reconstruction& r);

}
```

**Design constraints on the seam:**
- It operates only on canonical P2.5/P3 types; **no GTSAM headers** (D-AB-01/D-AB-02, as in the
  P3-impl-6 adapter boundary).
- The caller (engine/pipeline layer) is responsible for resolving the FrameGraph alignment and
  building `reconstruction_from_trajectory`; the seam applies it.
- It never mutates the source `Reconstruction` — it returns a new one (immutability, §7).

### 12.2 New/modified supporting types

- **`(new)` coordinate-frame declaration** (optional, recommended): the seam accepts an explicit
  `reconstruction_from_trajectory` (satisfies §5.4 recommendation (1)). No new canonical type is
  strictly required for this increment; the alignment is passed directly. If a persisted declaration
  is wanted later (§16.2) it would be a small provenance/`backend_specific_json` addition, not a new
  struct.

### 12.3 Schema validation function

- **`(new)`** `ValidateOptimizedReconstruction` above (or reuse the existing schema-validation
  mechanism — see below). The produced document must validate against `reconstruction.schema.json`
  (v2), with `status="succeeded"`, all `required` fields populated.
- If the platform already routes payload validation through a shared validator (schema-resident),
  the seam calls it; otherwise it constructs/validates the JSON per `reconstruction.schema.json`.

### 12.4 DB accessors

- **`(new)` UPDATE path on `ReconstructionRow`**: `MetadataDb` (`core/storage/metadata_db.h`) has
  `AddReconstruction` (insert) but **no** status-update method
  (`metadata_db.h:450`). Add e.g. `SetReconstructionStatus(const Uuid& id, std::string status)` to flip
  the prior row to `"superseded"` (§7.4). (This mirrors the status-UPDATE gap already flagged in
  P3-impl-7c §10.)

### 12.5 Files, line-level descriptions

| File | Change | Location |
|---|---|---|
| `core/trajectory/reconstruction_feedback.h` | **(new)** seam + input/output + validation | (new) |
| `core/trajectory/reconstruction_feedback.cpp` | **(new)** implementation (join, transform, write-back, determinism) | (new) |
| `core/storage/metadata_db.h` | add `SetReconstructionStatus` declaration | after `AddReconstruction`/`FindReconstructionsByScene` block `metadata_db.h:447-454` |
| `core/storage/metadata_db.cpp` | add `SetReconstructionStatus` definition (UPDATE `reconstruction_rows` SET `status`) | matching `.cpp` |
| `engine/pipeline/<new>_stage.{h,cpp}` | **(new)** orchestration: read OptimizationResult+Trajectory+Reconstruction, resolve frames/alignment, call `ApplyOptimizedTrajectory`, persist (insert new row, supersede old row) | (new) |
| `tests/unit/test_reconstruction_feedback.cpp` | **(new)** test suite (see §14) | (new) |

No changes are required to `reconstruction.h`, `trajectory.h`, `optimization.h`, `pose_graph.h`,
`frame.h`, the schemas, the GTSAM adapter, or the COLMAP adapter for the Option-A first increment.

---

## 13. Exact files involved (created or modified)

**Created (implementation phase):**
1. `core/trajectory/reconstruction_feedback.h`
2. `core/trajectory/reconstruction_feedback.cpp`
3. `engine/pipeline/<new>_stage.{h,cpp}` (name TBD at implementation)
4. `tests/unit/test_reconstruction_feedback.cpp`

**Modified (minimal):**
5. `core/storage/metadata_db.h` (one accessor declaration)
6. `core/storage/metadata_db.cpp` (one accessor definition)

**Unchanged (verified constraints):**
- `core/reconstruction/reconstruction.h`, `schemas/json/reconstruction.schema.json` — P2.5 canonical
  semantics preserved; no change unless §16 resolves differently.
- `core/trajectory/trajectory.h`, `optimization.h`, `pose_graph.h`, `trajectory_adapter.h`
- `adapters/gtsam/*`, `adapters/colmap/*`
- `core/scene/frame.h`, `core/geometry/*`, `core/coordinates/*`

---

## 14. Test matrix

| # | Test | What it verifies |
|---|---|---|
| 1 | `JoinBothPresent` | Trajectory node ↔ ReconImage matched by `frame_id`; optimized pose applied; cross-checked identities (`OptimizedPoseNode.frame_id == TrajectoryPoseNode.frame_id == ReconImage.frame_id`) |
| 2 | `JoinTrajNoImage` | Node with no matching ReconImage → skipped; no phantom image created (§9) |
| 3 | `JoinImageNoTraj` | ReconImage with no node → original pose preserved (§9) |
| 4 | `JoinDetectedFalse` | `detected=false` image → copied unchanged; never updated/relocalized (§9/§6) |
| 5 | `CoordinateFrameMismatch` | frames differ + no alignment → fail closed (typed error), nothing written (§5.4) |
| 6 | `CoordinateFrameIdentity` | same-frame (identity alignment) → raw `T_trajectory_camera` written, numerically unmodified (§5.2/§4.6) |
| 7 | `CoordinateFrameAligned` | frames differ + explicit `T_reconstruction_trajectory` → `T_rc = T_rt * T_tc` correct (§4.6) |
| 8 | `NoDoubleInversion` | `T_rt * T_tc` (not `T_rt * T_tc^-1`, not `T_rt^-1 * T_tc`) — golden numeric check on a non-trivial rotation (§4.5) |
| 9 | `UnmatchedNodes` | mixed matched/unmatched set → matched updated, unmatched preserved, order stable (§9) |
| 10 | `MissingFrameId` | empty `frame_id` on node (skip) and on image (keep pose); logs; continues (§10) |
| 11 | `DeterministicOutput` | two runs, identical inputs → identical `Reconstruction` document fields (modulo instance UUID) (§11) |
| 12 | `CasHashDeterminism` | identical inputs → identical content SHA-256; dedupe in `ArtifactStore::Put` yields `deduplicated=true` (§7.2/§11) |
| 13 | `SchemaValidation` | output validates against `reconstruction.schema.json`, `status="succeeded"`, all required fields (§12.3) |
| 14 | `OptionAPointsUnchanged` | `ReconPoint3D.xyz`, `track`, `ReconCamera`, `detected` byte-identical to source; only matching `ReconImage.pose` changed (§6) |
| 15 | `RevisionSemantics` | new `reconstruction_id`; source row status → `"superseded"`; new row `"succeeded"`; source document bytes immutable (§7) |
| 16 | `ProvenanceLineage` | optimized provenance `backend.name="spatial_optimizer"`, `input_artifact_hashes` include OptimizationResult hash + source Reconstruction hash; no self-reference / acyclic (§8) |
| 17 | `E2E` | full chain §15 end-to-end with numeric assertions (§15) |

---

## 15. E2E test scenario

**Scenario — COLMAP Reconstruction → Trajectory → Pose Graph → GTSAM → Optimized Trajectory → Reconstruction v2:**

1. **COLMAP Reconstruction (v1).** Parse a COLMAP `SparseModel` via `adapters/colmap/*` into a
   `Reconstruction` (P2.5), `status="succeeded"`, `coordinate_frame="reconstruction_0"`,
   `backend.name="colmap"`. Contains N `DetectedImage` images with `ReconImage.pose` =
   `T_reconstruction_camera` and M `ReconPoint3D` in the reconstruction frame.
2. **Trajectory (P3).** `SparseModelToTrajectory` (`adapters/colmap/colmap_trajectory_adapter.h:72`)
   → `Trajectory` (`kind="sfm"`, `coordinate_frame="trajectory_0"`,
   `TrajectoryPoseNode` = `T_trajectory_camera`, frame_id populated from the `frame_id_map`). Poses
   numerically equal v1's `ReconImage.pose` (same COLMAP run → same frame, §5.2).
3. **Pose Graph (P3).** `AssemblePoseGraph` (`core/trajectory/pose_graph_helpers.h:147`) → `PoseGraph`
   + nodes + odometry edges.
4. **GTSAM (P3-impl-6).** `spatial::adapters::gtsam::optimize(input, output)`
   (`gtsam_optimizer_adapter.h:64`) → `OptimizationResult` + `OptimizedPoseNode[]` (corrected
   `T_trajectory_camera`), ordered by `sequence_index`.
5. **Reconstruction v2 (this increment).** `ApplyOptimizedTrajectory` with
   `reconstruction_from_trajectory = Identity` (COLMAP same-model), source v1, `OptimizationResult`,
   `optimized_nodes`. Produces a **new** `Reconstruction`: same cameras/points/detected; `ReconImage.pose`
   updated from optimized nodes (in `reconstruction_0` frame); new `reconstruction_id`;
   `status="succeeded"`; provenance `"spatial_optimizer"` with OptimizationResult hash.
6. **Persist.** Insert new `ReconstructionRow`; update source row → `"superseded"`.

**Assertions:**
- The v1 CAS document bytes/hash are unchanged (immutability).
- `v2.points3D` and `v2.cameras` are byte-identical to v1; only updated `ReconImage.pose` values differ
  and equal the corrected trajectory poses (identity alignment).
- `v2.reconstruction_id != v1.reconstruction_id`; `v2.status == "succeeded"`; source row now
  `"superseded"`.
- Provenance chain is acyclic and reaches COLMAP; `backend.name == "spatial_optimizer"`.
- If drift was induced (loop closure not used), optimized endpoint error < original endpoint error
  (this is the P3-impl-6 drift-correction result reflected into v2 poses) and matches the
  `OptimizationResult` error metrics within tolerance.
- `v2` validates against `reconstruction.schema.json`.
- Determinism: rerun → same CAS hash.

**Boundary (must be stated):** this increment proves *pose-only feedback correctness and determinism*;
it does not assert absolute metric accuracy, sub-metre grounding, or multi-session alignment (those
are future increments).

---

## 16. Blockers / unresolved decisions

### 16.1 Coordinate-frame declaration (needed before robust implementation)

The trajectory payload (`trajectory.schema.json`) and `OptimizationResult` payload
(`optimization-result.schema.json`) carry no explicit statement of *which* Reconstruction's frame
they share. The same-frame property is currently an *implication* of the COLMAP trajectory adapter
inverting the same model (§5.2). For the first increment this is acceptable if the **caller is
required to supply `T_reconstruction_trajectory` explicitly (default Identity)** — Recommendation
(1). A persisted declaration (Recommendation (2)) is a cleaner long-term fix but adds schema surface.

**Impact:** none for the recommended path; it is a caller-contract the seam enforces (test #5).

### 16.2 `backend.name` vocabulary

Recommendation `"spatial_optimizer"` (§8.2) introduces a backend-name string not currently present in
the documented P2.5 examples (`reconstruction.schema.json:285` lists `'colmap', 'orbslam', 'hybrid'`).
`ReconstructionProvenance.backend.name` is a free-form string (`reconstruction.schema.json:282-286`),
so no schema change is required — but the maintainers should ratify the new vocabulary entry and
record it in the P2.5 doc.

### 16.3 DB status UPDATE path

`MetadataDb` currently only INSERTs reconstruction rows (`AddReconstruction`, `metadata_db.h:450`).
Superseding the prior row requires an UPDATE (`SetReconstructionStatus`). This is a small, bounded
DB-layer addition consistent with the UPDATE-gap already noted for pose-graph/trajectory status in
P3-impl-7c §10.

### 16.4 Option A frame-mixing caveat (explicitly stated)

Under Option A, if the trajectory and reconstruction frames **differ** and an alignment is applied,
then `ReconImage.pose` (now in the reconstruction frame via alignment) and `ReconPoint3D.xyz`
(original frame) remain **frame-consistent** only because the reconstruction frame is unchanged. The
points were never transformed; the alignment only expresses the corrected camera poses in that same
unchanged frame. Therefore **no frame-mixing inconsistency arises**: both stay in
`reconstruction_0`. The only way inconsistency could appear is if the alignment itself were wrongly
conflated with a point transform (it is not — points are untouched). If a later increment recomputes
points (full BA), those new points would be in the optimized trajectory's frame and would need an
explicit one-time rigid transform into the reconstruction frame before write-back — that is **out of
scope** for this increment and is flagged for future increments (§17).

---

## 17. Non-goals / deferred

- ORB-SLAM3 / KISS-ICP adapters (P3-impl-4/9).
- Loop-closure-specific *implementation* (that is the other `impl-7a/7b/7c` thread); this increment
  merely *consumes* an already-optimized trajectory.
- 3D-point recomputation / full bundle adjustment on the optimized trajectory (future).
- Multi-session alignment, map merge, uncertainty propagation to points.
- New P2.5 canonical semantics: unchanged unless §16.1 resolves a genuine architectural conflict.

---

## 18. Scope guardrails

**This mapping does NOT authorize:**
- Any code, schema, DB-migration, or build changes (all listed in §12/§13 are for the implementation
  phase).
- Modifying P2.5 canonical types/schemas or P3 canonical types unless §16 demonstrates a genuine
  conflict (currently it does not).
- Adding new reconstruction backends or loop-closure-implementation work.

---

## 19. Verdict

### **READY TO PROCEED TO IMPLEMENTATION (Option A, pose-only first increment)**

All 15 required content areas are mapped with exact type/file/schema references. The single
architectural decision that must be ratified before coding is **§16.1 — the explicit
`T_reconstruction_trajectory` (default Identity) caller contract**; the recommended resolution is
adopted in this mapping. On that ratification, implementation follows §12/§13 with the §14 test
matrix and §15 E2E.

**Proposed next step:** ratify §16.1 (and the §8.2 backend-name entry), then authorize implementation
of `core/trajectory/reconstruction_feedback.*` + the metadata-db status UPDATE + the engine stage +
tests.

---

*This mapping was produced by opencode on 2026-09-02. No code, schema, dependency, or build changes
were made.*
