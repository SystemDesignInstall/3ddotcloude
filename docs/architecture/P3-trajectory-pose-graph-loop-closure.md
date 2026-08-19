# P3 — Trajectory / Pose Graph / Loop Closure Architecture

**Status:** Design — pending architectural approval  
**Scope:** Architecture/design increment only (no code changes)  
**Depends on:** P2.5 COMPLETE, C1 COMPLETE  
**Date:** 2026-08-18

---

## 1. Problem Statement

Spatial Platform has completed C1 (execution architecture) and P2.5 (Canonical Reconstruction Model v2). COLMAP now produces backend-independent reconstructions through an adapter seam.

The next capability to add is **long-sequence trajectory handling with drift correction**:

1. A camera moving through space accumulates local odometry. Over long sequences, odometry drifts.
2. When the camera revisits an already observed area, the system must detect this **loop closure**, add a global constraint, optimize the trajectory, and produce corrected poses.
3. The corrected trajectory must feed into Canonical Reconstruction v2.
4. The optimization backend (e.g. GTSAM) must be replaceable without affecting Core.

This design defines how Spatial Platform represents, computes, validates, optimizes, and persists camera trajectories and pose-graph constraints **independently of any specific SLAM/backend implementation**.

---

## 2. Architectural Goals

1. **Backend-independent trajectory representation.** A trajectory is expressed in canonical types regardless of whether the backend is ORB-SLAM, KISS-ICP, COLMAP, or a future AI system.

2. **Backend-replaceable optimization.** The optimizer (GTSAM, Ceres, custom) is isolated behind an adapter seam.

3. **Loop closure as a semantic contract.** The platform defines what a loop closure *is* and what it *produces*, not how it is detected.

4. **Original poses preserved.** Odometry poses are never destroyed by optimization. The original trajectory remains queryable.

5. **Reconstruction v2 remains authoritative.** Optimized trajectory produces `ReconImage.pose` inside Canonical Reconstruction v2. The trajectory is an intermediate representation that feeds reconstruction, not a replacement for it.

6. **Multi-session support.** Trajectories from multiple sessions can be aligned and fused within a scene.

7. **Uncertainty propagation.** Pose-graph optimization changes uncertainty. This change must be tracked and propagated.

8. **Provenance lineage.** The full chain from raw observations → candidates → verification → edge → optimization → reconstruction must be traceable.

---

## 3. Non-Goals

1. **No SLAM implementation.** This is a design for the data model and adapter boundary. SLAM execution is deferred to a future implementation increment.

2. **No GTSAM integration.** GTSAM is the planned optimizer (ADR-005), but this design defines the boundary, not the GTSAM adapter code.

3. **No KISS-ICP integration.** LiDAR odometry is out of scope for this design.

4. **No hybrid fusion.** Combining SLAM and SfM in a single reconstruction is a future increment.

5. **No AI/learned loop closure.** ADR-006 applies: AI outputs are priors, never authoritative. Classical pipeline first.

6. **No final JSON schemas.** The artifact model is designed here; schemas are created during implementation.

7. **No Engine/Scheduler/Worker protocol changes.** The existing execution model is sufficient for trajectory backends.

8. **No calibration work.** Calibration is orthogonal to trajectory design.

9. **No dense reconstruction.** Dense/mesh/textured outputs are separate pipeline stages.

10. **No code changes.** This is a pure design document.

---

## 4. Existing Architecture Integration

### 4.1 Key Existing Entities

| Entity | Location | Role in P3 |
|--------|----------|------------|
| `Frame` | `core/scene/frame.h` | Kinematic frame for one exposure. Has `pose_ref` (nil at import, resolved by trajectory). |
| `CaptureSession` | `core/scene/capture_session.h` | Groups frames captured in one pass. Trajectory belongs to a session. |
| `Scene` | `core/scene/scene.h` | Central domain object. Trajectories and reconstructions belong to a scene. |
| `SceneVersion` | `core/scene/scene.h` | Immutable append-only scene state. Trajectory creation produces a new version. |
| `FeatureSet` | `core/scene/feature/feature_set.h` | Per-frame keypoints/descriptors. Required for loop closure feature matching. |
| `ImageObservation` | `core/scene/observation_graph/image_observation.h` | Immutable observation record. Part of the observation graph (ADR-024). |
| `Reconstruction` | `core/reconstruction/reconstruction.h` | Canonical Reconstruction v2. The final output that optimized trajectory feeds. |
| `ReconPose` | `core/reconstruction/reconstruction.h` | `T_reconstruction_camera` — what the trajectory ultimately produces. |
| `FrameGraph` | `core/coordinates/frame_graph.h` | DAG of `CoordinateFrame` nodes with `RigidTransform` edges. Trajectory frame is a node. |
| `SE3`, `RigidTransform`, `WorldFromCamera` | `core/geometry/` | Pose types. Trajectory uses SE3; FrameGraph uses RigidTransform. |
| `ArtifactStore` (CAS) | `core/artifacts/artifact_store.h` | Content-addressed storage. Trajectory payloads are CAS artifacts. |
| `MetadataDb` | `core/storage/metadata_db.h` | SQLite metadata. Trajectory metadata stored here; payloads in CAS. |

### 4.2 Key Existing Decisions

| Decision | Source | Implication for P3 |
|----------|--------|---------------------|
| ADR-005: GTSAM as unified factor graph | ADR-005 | Pose graph is GTSAM-native concept; canonical model must be GTSAM-compatible |
| ADR-024: Observation Graph is canonical substrate | ADR-024 | Loop closures reference observations, not raw images |
| ADR-007: Coordinate frame conventions | ADR-007 | All poses use right-handed, meters, nanoseconds, SE(3), scalar-last quaternion |
| ADR-018: Strict scalar types | ADR-018 | Typed transform pairs; no raw Eigen in domain code |
| D-CRM-01: T_reconstruction_camera | P2.5 | Optimized trajectory must produce this, not T_world_camera |
| D-CRM-19: Each SLAM snapshot is a new Reconstruction | P2.5 | Trajectory versioning aligns with Reconstruction snapshotting |
| RFC-0002: Observations are permanent | RFC-0002 | Pose graph edges reference immutable observations |
| ADR-006: AI outputs are priors | ADR-006 | Classical loop closure first; AI augmentation deferred |

### 4.3 Existing Frame Graph Hierarchy (from P2.5)

```
project_world (root)
    ├── reconstruction_0
    │     ├── camera_0
    │     ├── camera_1
    │     └── ...
    ├── reconstruction_1
    └── trajectory_0  (SLAM trajectory node — reserved in P2.5)
```

P3 defines what `trajectory_0` contains and how it relates to the other nodes.

---

## 5. Canonical Terminology

| Term | Definition |
|------|-----------|
| **Trajectory** | An ordered sequence of camera poses through time, produced by a single processing session (odometry, SLAM, or SfM). |
| **Pose Node** | A single pose in a trajectory: timestamp + SE(3) transform + optional covariance. |
| **Pose Edge** | A constraint between two pose nodes: relative transform + information matrix. |
| **Odometry Edge** | A pose edge derived from consecutive-frame motion estimation (local, high-confidence short-range). |
| **Loop Closure Edge** | A pose edge derived from recognizing a previously visited location (global, long-range constraint). |
| **Prior Edge** | A pose edge anchoring a node to a known position (e.g. GNSS, fixed origin). |
| **Pose Graph** | The graph structure: nodes are poses, edges are constraints. The input to optimization. |
| **Loop Closure** | The process of detecting that the camera has revisited a previously observed area. |
| **Candidate** | A potential loop closure pair, before geometric verification. |
| **Closure** | An accepted loop closure — a verified, high-confidence pose edge. |
| **Optimization** | The process of adjusting pose nodes to minimize global error over all edges. |
| **Acquisition Trajectory** | The original trajectory as produced by odometry/SLAM, before loop closure optimization. |
| **Optimized Trajectory** | The trajectory after pose-graph optimization. May have reduced uncertainty. |
| **Registration** | Aligning two coordinate frames (e.g. two sessions, or trajectory to project_world). |
| **Drift** | Accumulated error in odometry, causing poses to diverge from ground truth over time. |

---

## 6. Trajectory Model

### 6.1 Design Decision: Trajectory Entity

**D-TRJ-01:** A Trajectory is a **standalone CAS artifact with a metadata row**, not embedded in Scene or Reconstruction.

**Rationale:**
- A trajectory is a substantial data structure (potentially thousands of poses with covariances).
- It needs its own lifecycle (created, optimizing, optimized, superseded).
- Multiple trajectories can exist per session (original + optimized).
- CAS storage is consistent with how Reconstruction, FeatureArtifact, and Calibration are stored.
- The metadata row enables efficient queries (latest trajectory per session) without loading the full payload.

### 6.2 Trajectory Entity

```cpp
struct Trajectory {
  std::string trajectory_id;       // UUIDv4, instance-scoped (D-TRJ-02)
  std::string scene_id;            // owning scene
  std::string session_id;          // the capture session this trajectory belongs to
  std::string kind;                // "odometry" | "slam" | "sfm" | "survey" (D-TRJ-03)
  std::string coordinate_frame;    // e.g. "trajectory_0" (D-TRJ-04)
  std::string status;              // "building" | "optimized" | "superseded" (D-TRJ-05)
  int64_t created_at_ns;           // creation timestamp
  int64_t node_count;              // number of pose nodes (for quick display)
  double total_distance_m;         // path length in metres (for quick display)
  double total_duration_ns;        // time span in nanoseconds
  ReconstructionProvenance provenance;  // reuse P2.5 provenance type
  TrajectoryUncertainty uncertainty;     // aggregate uncertainty metrics (D-TRJ-06)
};
```

**D-TRJ-02:** Trajectory IDs are UUIDv4 (instance-scoped, not content-derived). Rationale: a trajectory is an instance of processing, not an immutable content record. Re-running the same SLAM produces a new trajectory with a new ID.

**D-TRJ-03:** `kind` is one of:
- `"odometry"` — visual odometry or incremental SLAM output
- `"slam"` — full SLAM with loop closure already applied by the backend
- `"sfm"` — Structure-from-Motion trajectory (COLMAP-style)
- `"survey"` — GNSS/RTK trajectory

**D-TRJ-04:** `coordinate_frame` follows the same naming convention as Reconstruction (D-CRM-06). Each trajectory defines its own local frame. Alignment to `project_world` is a separate step.

**D-TRJ-05:** Lifecycle: `building` → `optimized` (after pose-graph optimization) → `superseded` (when a newer trajectory replaces this one).

**D-TRJ-06:** Aggregate uncertainty is a summary, not per-pose detail. Per-pose uncertainty lives in the pose payload.

### 6.3 Pose Payload (CAS Document)

The per-pose data lives in a CAS payload referenced by the trajectory. This keeps the metadata row lightweight.

```json
{
  "schema_version": 1,
  "trajectory_id": "<uuid>",
  "nodes": [
    {
      "frame_id": "<uuid>",
      "timestamp_ns": 1783123200000000000,
      "sequence_index": 0,
      "position_xyz": [1.0, 2.0, 3.0],
      "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
      "covariance_position": [0.01, 0.0, 0.0, 0.01, 0.0, 0.01],
      "covariance_rotation": [0.001, 0.0, 0.0, 0.001, 0.0, 0.001]
    }
  ]
}
```

**D-TRJ-07:** Pose payload nodes are ordered by `sequence_index` (monotonically increasing, gaps allowed for dropped frames). Timestamps are monotonic but not required to be equally spaced.

**D-TRJ-08:** Position and rotation are stored as **world_from_body** (or **trajectory_from_body**) — the transform that maps a point from the camera/body frame into the trajectory's local coordinate frame. This is consistent with `T_reconstruction_camera` (D-CRM-01).

**D-TRJ-09:** Covariance is optional per node. When absent, the pose is treated as having unknown uncertainty. Covariance uses the platform convention: translation-then-rotation ordering (ADR-007).

### 6.4 Relationship to Frame

```
Frame
  ├── frame_id        (UUIDv5, deterministic)
  ├── timestamp_ns
  ├── sequence_index
  ├── sensor_id
  └── pose_ref        (nil at import; resolved by TrajectoryAdapter)

Trajectory PoseNode
  ├── frame_id        → Frame.frame_id  (bridge, D-TRJ-10)
  ├── timestamp_ns    (must match Frame.timestamp_ns)
  └── position/rotation
```

**D-TRJ-10:** Trajectory nodes reference `Frame` by `frame_id`. This is a **bridge reference** (UUID string), not an embedded copy. The Frame's own `pose_ref` field may be populated by the trajectory adapter as a back-pointer (optional, see Section 11).

### 6.5 Relationship to CaptureSession

A Trajectory belongs to exactly one CaptureSession (`session_id`). A CaptureSession may have multiple trajectories (e.g. odometry + optimized). The latest non-superseded trajectory is the "active" trajectory for that session.

### 6.6 Relationship to Scene

A Trajectory belongs to exactly one Scene (`scene_id`). Multiple sessions' trajectories coexist within a scene. Alignment to a common coordinate frame (`project_world`) is a separate operation.

---

## 7. Pose Graph Model

### 7.1 Design Decision: Pose Graph Entity

**D-PG-01:** The PoseGraph is a **separate CAS artifact**, not embedded in Trajectory.

**Rationale:**
- A pose graph may contain edges from multiple sources (odometry + loop closure + GNSS).
- Optimization reads the graph and produces a result — this is a clean input/output contract.
- The graph is mutable (edges are added during loop closure detection) while the trajectory is versioned.
- Separating graph from trajectory allows the same graph to be optimized multiple times with different settings.

### 7.2 PoseGraph Entity

```cpp
struct PoseGraph {
  std::string graph_id;             // UUIDv4
  std::string trajectory_id;        // the trajectory this graph was built from
  std::string scene_id;
  std::string status;               // "building" | "ready" | "optimizing" | "optimized" (D-PG-02)
  int64_t node_count;
  int64_t edge_count;
  int64_t odometry_edge_count;
  int64_t loop_closure_edge_count;
  int64_t prior_edge_count;
  int64_t created_at_ns;
  ReconstructionProvenance provenance;
};
```

**D-PG-02:** PoseGraph lifecycle: `building` → `ready` (all edges added) → `optimizing` → `optimized` (or `failed`).

### 7.3 Pose Graph Payload (CAS Document)

```json
{
  "schema_version": 1,
  "graph_id": "<uuid>",
  "trajectory_id": "<uuid>",
  "nodes": [
    {
      "node_id": 0,
      "frame_id": "<uuid>",
      "timestamp_ns": 1783123200000000000
    }
  ],
  "edges": [
    {
      "edge_id": 0,
      "type": "odometry",
      "source_node_id": 0,
      "target_node_id": 1,
      "relative_position_xyz": [0.1, 0.0, 0.0],
      "relative_rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
      "information_matrix_6x6": [1000, 0, 0, 0, 1000, 0, 0, 0, 1000, 100, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
      "confidence": 0.95,
      "provenance": {
        "source": "visual_odometry",
        "configuration_hash": "<hash>"
      }
    }
  ]
}
```

**D-PG-03:** Nodes carry only `node_id` (integer), `frame_id` (bridge to Frame), and `timestamp_ns`. Pose data is NOT duplicated here — it lives in the Trajectory payload. The graph only needs the node ordering and connectivity.

**D-PG-04:** Edge `type` is one of: `"odometry"`, `"loop_closure"`, `"prior"`, `"gps"`, `"imu_preintegration"`, `"lidar_odometry"`.

**D-PG-05:** The `information_matrix_6x6` is flattened row-major in translation-then-rotation ordering (ADR-007). This is the inverse of the covariance. High information = high confidence = tight constraint.

**D-PG-06:** Each edge carries a `confidence` score in [0, 1] for human-facing quality assessment. The information matrix is the mathematical weight used by the optimizer; confidence is informational only.

**D-PG-07:** Edge provenance tracks the source (which algorithm produced the edge) and configuration hash. This enables reproducibility and debugging.

### 7.4 Relationship to FrameGraph

The PoseGraph is **orthogonal to the FrameGraph** (core/coordinates):

- **FrameGraph** represents *static spatial relationships* between coordinate frames (rigid transforms, sensor mounts). It answers: "What is the transform from camera to rig?"
- **PoseGraph** represents *temporal constraints* between camera poses. It answers: "What is the relative motion between frame t₁ and frame t₂?"

The PoseGraph and FrameGraph may share `FrameId` references but serve different purposes. After optimization, the corrected transforms can be installed into the FrameGraph (see Section 12).

---

## 8. Loop Closure Model

### 8.1 Semantic Contract

**D-LC-01:** Loop closure is defined as a **backend-independent semantic contract**, not a specific algorithm. The platform specifies:

1. What inputs loop closure requires.
2. What outputs it produces.
3. What properties accepted closures must satisfy.

The specific detection algorithm (boW, NetVLAD, SuperGlue, COLMAP matching) is chosen by the backend adapter.

### 8.2 Loop Closure Pipeline (Semantic)

```
FeatureArtifacts (from Frame → FeatureSet → CAS)
        ↓
Candidate Generation
  "Do these two frames look similar?"
        ↓
Geometric Verification
  "Are the matched features geometrically consistent?"
        ↓
Accepted / Rejected
        ↓
PoseGraph Edge (if accepted)
```

### 8.3 Candidate

**D-LC-02:** A loop closure candidate is a **potential match** between two frames, before geometric verification. Candidates are lightweight and may be numerous.

```cpp
struct LoopClosureCandidate {
  std::string candidate_id;       // UUIDv4
  std::string trajectory_id;
  std::string source_frame_id;    // the newer frame
  std::string target_frame_id;    // the older frame (the "revisited" location)
  double feature_match_score;     // raw matching score [0, ∞), backend-specific
  std::string matcher;            // which matcher produced this (e.g. "bow", "netvlad")
  int64_t created_at_ns;
};
```

**D-LC-03:** Candidates are **persisted** (not just accepted closures). This enables:
- Debugging why a closure was rejected.
- Re-evaluation with different thresholds.
- Audit trail for quality assurance.

### 8.4 Geometric Verification

**D-LC-04:** Geometric verification takes a candidate and produces a **verified closure** or **rejection**. The verification must check:

1. **Sufficient inlier count** — enough feature matches agree with a geometric model.
2. **Inlier ratio** — the proportion of matches that are geometrically consistent.
3. **Epipolar consistency** — matched points satisfy the fundamental/essential matrix.
4. **Spatial consistency** — the proposed transform is physically plausible (not a mirror, not impossibly far).

### 8.5 Accepted Closure

```cpp
struct LoopClosure {
  std::string closure_id;         // UUIDv4
  std::string trajectory_id;
  std::string candidate_id;       // back-reference to the candidate
  std::string source_frame_id;
  std::string target_frame_id;
  std::string status;             // "accepted" | "rejected" (D-LC-05)
  double inlier_ratio;            // fraction of geometrically consistent matches
  int64_t inlier_count;           // absolute number of inliers
  double confidence;              // combined confidence [0, 1]
  int64_t temporal_separation_ns; // |timestamp_source - timestamp_target|
  double spatial_separation_m;    // Euclidean distance between original poses
  int64_t created_at_ns;
  ReconstructionProvenance provenance;
};
```

**D-LC-05:** Closures have status `"accepted"` or `"rejected"`. Both are persisted for audit.

**D-LC-06:** `temporal_separation_ns` and `spatial_separation_m` are metadata for quality assessment and duplicate detection. A closure with very small temporal separation is likely a consecutive pair (not a true loop). A closure with very large spatial separation may indicate a false positive.

### 8.6 Duplicate Closures

**D-LC-07:** Duplicate closures are detected by checking if the same `(source_frame_id, target_frame_id)` pair already has an accepted closure. If so, the new candidate is rejected as a duplicate.

**D-LC-08:** Near-duplicates (same pair within a small temporal window) are also rejected. The window is configurable per backend.

### 8.7 False-Positive Protection

**D-LC-09:** False-positive protection relies on:

1. **Geometric verification** (D-LC-04) — the primary defense.
2. **Minimum inlier threshold** — configurable per backend.
3. **Minimum confidence** — below this threshold, the closure is rejected.
4. **Spatial consistency check** — the proposed relative transform must be consistent with the existing trajectory (within a configurable tolerance).

### 8.8 Loop Closure Output

A confirmed loop closure produces:
1. A `LoopClosure` record with status `"accepted"`.
2. A `PoseGraph` edge of type `"loop_closure"`.

---

## 9. Optimization Model

### 9.1 Design Decision: Optimization Contract

**D-OPT-01:** Optimization is defined as a **clean input/output contract**:

- **Input:** PoseGraph (nodes + edges + information matrices).
- **Output:** Optimized poses + quality metrics + optimization provenance.
- **Mechanism:** Adapter-specific (GTSAM, Ceres, custom). The optimizer is a replaceable backend.

### 9.2 Optimization Input/Output

```cpp
struct OptimizationInput {
  std::string graph_id;              // the PoseGraph to optimize
  std::string trajectory_id;         // the original trajectory
  std::string config_json;           // optimizer-specific settings (max iterations, convergence threshold, etc.)
  std::string configuration_hash;    // SHA-256 of config_json for caching
};

struct OptimizationResult {
  std::string result_id;             // UUIDv4
  std::string graph_id;
  std::string trajectory_id;
  std::string status;                // "converged" | "failed" | "diverged" (D-OPT-02)
  int64_t iterations;                // actual iterations performed
  double initial_error;              // total squared error before optimization
  double final_error;                // total squared error after optimization
  double error_reduction;            // (initial - final) / initial, in [0, 1]
  int64_t created_at_ns;
  OptimizationProvenance provenance;
};
```

**D-OPT-02:** Optimization status:
- `"converged"` — optimizer reached convergence criteria.
- `"failed"` — optimizer hit iteration limit or numerical issues without converging.
- `"diverged"` — error increased (rare, indicates model inconsistency).

### 9.3 Optimized Trajectory

**D-OPT-03:** Optimization produces a **new optimized trajectory payload** (not a mutation of the original).

```cpp
struct OptimizedTrajectoryPayload {
  std::string schema_version;       // "1"
  std::string trajectory_id;        // the original trajectory being optimized
  std::string optimization_result_id;
  std::vector<OptimizedPoseNode> nodes;
};

struct OptimizedPoseNode {
  std::string frame_id;
  int64_t timestamp_ns;
  int64_t sequence_index;
  std::array<double, 3> position_xyz;     // corrected position
  std::array<double, 4> rotation_xyzw;    // corrected rotation
  std::array<double, 6> covariance_position;   // post-optimization covariance (D-OPT-04)
  std::array<double, 6> covariance_rotation;
};
```

**D-OPT-04:** Post-optimization covariance reflects the optimizer's estimate of pose uncertainty after incorporating all constraints. Covariances typically decrease after loop closure (the trajectory is more certain). When the optimizer does not produce covariances, these fields are zero-filled.

### 9.4 Original Poses Preserved

**D-OPT-05:** The original trajectory payload is **never modified** by optimization. The optimized poses are stored as a separate CAS document. Both are queryable:
- Original: referenced by `Trajectory.trajectory_id`
- Optimized: referenced by `OptimizationResult` and linked to the `Trajectory`

**D-OPT-06:** The `Trajectory.status` transitions from `"building"` to `"optimized"` after successful optimization. This indicates that an optimized payload exists, not that the original was replaced.

### 9.5 Quality Metrics

| Metric | Description |
|--------|-------------|
| `initial_error` | Total squared error before optimization (reprojection + edge residuals) |
| `final_error` | Total squared error after optimization |
| `error_reduction` | Fraction of error removed: `(initial - final) / initial` |
| `max_node_correction` | Maximum pose correction (distance) applied to any single node |
| `mean_node_correction` | Mean pose correction across all nodes |
| `edge_consistency` | Fraction of edges with residual below threshold after optimization |

---

## 10. Coordinate-Frame Semantics

### 10.1 Frame Hierarchy After Trajectory

```
project_world (root)
    ├── trajectory_0          (trajectory local frame)
    │     ├── camera_0_from_trajectory_0
    │     ├── camera_1_from_trajectory_0
    │     └── ...
    ├── reconstruction_0      (reconstruction local frame)
    │     ├── camera_0_from_reconstruction_0
    │     └── ...
    └── trajectory_1          (second session's trajectory)
```

### 10.2 Trajectory Frame

**D-CF-01:** Each trajectory defines its own local coordinate frame (named by `coordinate_frame`, e.g. `"trajectory_0"`). This frame's origin is arbitrary (often the first pose or the odometry origin).

**D-CF-02:** Trajectory poses are expressed as `T_trajectory_camera` — the transform from camera frame to trajectory frame. This is analogous to `T_reconstruction_camera` (D-CRM-01) but in the trajectory's own frame.

### 10.3 Alignment to Project World

**D-CF-03:** Aligning a trajectory to `project_world` is a **separate operation** that installs a `RigidTransform` edge in the FrameGraph between `project_world` and the trajectory's frame node.

```
project_world → trajectory_0: T_world_trajectory (alignment transform)
```

**D-CF-04:** Alignment may be:
- **Identity** — the trajectory is already in the project world frame.
- **Computed** — estimated from GNSS, known markers, or manual registration.
- **Unknown** — the identity placeholder is used until alignment is performed.

### 10.4 Trajectory → Reconstruction Transform

**D-CF-05:** After optimization and alignment, the trajectory-to-reconstruction transform is:

```
T_reconstruction_camera = T_reconstruction_trajectory * T_trajectory_camera
```

Where `T_reconstruction_trajectory` is the alignment transform between the trajectory frame and the reconstruction frame (stored in the FrameGraph).

**D-CF-06:** This transform is computed by the adapter and applied during Reconstruction construction. The Reconstruction never absorbs the trajectory frame — it remains a separate coordinate frame in the FrameGraph.

---

## 11. Frame Integration

### 11.1 Frame ↔ Trajectory Node

```
Frame (import model)
  ├── frame_id: UUIDv5 (deterministic)
  ├── timestamp_ns: TimestampNs
  ├── sequence_index: int64
  ├── sensor_id: UUID
  └── pose_ref: UUID (nil at import)

Trajectory PoseNode
  ├── frame_id: string (UUID, → Frame.frame_id)
  ├── timestamp_ns: must match Frame.timestamp_ns
  └── position/rotation: SE(3)
```

**D-FI-01:** The trajectory node references Frame by `frame_id` (bridge). Frame does NOT embed pose data — it carries `pose_ref` which optionally points to the trajectory.

**D-FI-02:** `Frame.pose_ref` is **optional** and **resolved by the trajectory adapter**. When a trajectory is produced for a session, the adapter may populate `pose_ref` with the trajectory's `trajectory_id`. This is a convenience pointer, not the authoritative pose source. The authoritative source is always the Trajectory payload.

### 11.2 Frame ↔ Reconstruction Image

```
ReconImage
  ├── image_id: uint32 (backend-local)
  ├── frame_id: string (UUID, → Frame.frame_id)  [optional]
  └── pose: ReconPose (T_reconstruction_camera)
```

The bridge is already established in P2.5 (D-CRM-18): `ReconImage.frame_id` links to `Frame`. The trajectory provides the path from Frame to pose.

### 11.3 Frame ↔ FeatureSet ↔ FeatureArtifact

```
Frame → FeatureSet (per detector) → FeatureArtifact (CAS)
```

Loop closure operates on FeatureArtifacts (feature matching). The trajectory provides the temporal ordering that enables efficient candidate generation (search within temporal windows).

### 11.4 Full Integration Chain

```
Frame (import)
  │
  ├── FeatureSet → FeatureArtifact (features for loop closure matching)
  │
  ▼
Trajectory (poses through time)
  │
  ├── PoseGraph (constraints: odometry + loop closure + prior)
  │
  ▼
Optimization (corrected poses)
  │
  ▼
Reconstruction v2 (ReconImage.pose = T_reconstruction_camera)
```

---

## 12. Reconstruction v2 Integration

### 12.1 How Optimized Trajectory Becomes ReconImage.pose

**D-RI-01:** The trajectory-to-reconstruction conversion follows the same pattern as the COLMAP adapter (P2.5):

1. The adapter reads the optimized trajectory payload.
2. For each trajectory node, it resolves `frame_id → Frame → ImageObservation → Reconstruction image`.
3. It computes `T_reconstruction_camera = T_reconstruction_trajectory * T_trajectory_camera`.
4. It writes `ReconImage.pose = T_reconstruction_camera`.

**D-RI-02:** `T_reconstruction_camera` is preserved. The system does NOT convert it to `T_world_camera`. If the consumer needs `T_world_camera`, they compose: `T_world_camera = T_world_reconstruction * T_reconstruction_camera`.

### 12.2 Reconstruction Provenance

**D-RI-03:** When a trajectory feeds a reconstruction, the `ReconstructionProvenance` records:
- `backend.name` = the trajectory backend (e.g. `"orb_slam3"`)
- `input_artifact_hashes` includes the trajectory CAS hash and optimization result CAS hash.
- `backend_specific_json` may include trajectory-specific metadata (loop closure count, optimization iterations, etc.).

### 12.3 Reconstruction Snapshotting (D-CRM-19 Extended)

**D-RI-04:** Each trajectory optimization produces a **new Reconstruction snapshot** (consistent with D-CRM-19). The previous reconstruction's status becomes `"superseded"`. This ensures:
- The original reconstruction (from unoptimized trajectory) is preserved.
- The optimized reconstruction is the new authoritative result.
- Quality can be compared between snapshots.

---

## 13. Multi-Session Semantics

### 13.1 Trajectories per Session

**D-MS-01:** Each CaptureSession has its own trajectory (or set of trajectories). Sessions are independent — they may use different sensors, different backends, or different coordinate frames.

```
Scene
  ├── Session A → Trajectory A (odometry)
  ├── Session B → Trajectory B (odometry)
  └── Session C → Trajectory C (survey/GNSS)
```

### 13.2 Multi-Session Reconstruction

**D-MS-02:** A Reconstruction may span multiple sessions via `session_ids: UUID[]` (D-CRM-12). The reconstruction merges poses from all contributing trajectories into a single coordinate frame.

### 13.3 Alignment Between Sessions

**D-MS-03:** Aligning trajectories from different sessions is done through:
1. **Feature-based registration** — matching features between sessions to compute `T_trajectoryA_trajectoryB`.
2. **GNSS/known-point alignment** — using shared survey references.
3. **Manual alignment** — user-provided transform.

The alignment transform is installed in the FrameGraph as an edge between the two trajectory frame nodes.

### 13.4 Future: Cross-Session Loop Closure

**D-MS-04:** Cross-session loop closure (detecting that Session A and Session B observed the same area) is a **future capability**. The current design supports it semantically (the PoseGraph can contain edges between nodes from different trajectories) but does not define the cross-session candidate generation algorithm.

---

## 14. Artifact Model

### 14.1 Artifact Types

| Artifact | Type string | Schema version | Key contents |
|----------|------------|----------------|--------------|
| Trajectory | `"trajectory"` | 1 | Pose nodes (frame_id, timestamp, position, rotation, covariance) |
| PoseGraph | `"pose_graph"` | 1 | Nodes (frame_id, timestamp) + edges (type, relative transform, information matrix) |
| LoopClosure | `"loop_closure"` | 1 | Candidate + verification result (accepted/rejected, inlier stats, confidence) |
| OptimizationResult | `"optimization_result"` | 1 | Status, error metrics, optimized trajectory reference |

### 14.2 Artifact Manifests

All artifacts follow the existing `ArtifactManifest` pattern (ADR-010):

```json
{
  "artifact_uuid": "<uuid>",
  "content_hash": "<sha256>",
  "type": "trajectory",
  "schema_version": 1,
  "producer": {
    "id": "spatial-platform",
    "version": "0.3.0",
    "git_commit": "<commit>"
  },
  "input_artifact_hashes": ["<feature_artifact_hash>", "<config_hash>"],
  "configuration_hash": "<hash>",
  "coordinate_frame": "trajectory_0",
  "unit": "metres"
}
```

### 14.3 DB Metadata

Trajectory and PoseGraph metadata rows in `MetadataDb` enable fast queries without loading full payloads:

```sql
CREATE TABLE trajectories (
  trajectory_id    BLOB PRIMARY KEY,
  scene_id         BLOB NOT NULL,
  session_id       BLOB NOT NULL,
  kind             TEXT,
  coordinate_frame TEXT,
  status           TEXT,
  node_count       INTEGER,
  total_distance_m REAL,
  total_duration_ns INTEGER,
  created_at_ns    INTEGER,
  document_json    TEXT              -- full Trajectory as JSON (like Reconstruction)
);

CREATE TABLE pose_graphs (
  graph_id       BLOB PRIMARY KEY,
  trajectory_id  BLOB NOT NULL,
  scene_id       BLOB NOT NULL,
  status         TEXT,
  node_count     INTEGER,
  edge_count     INTEGER,
  created_at_ns  INTEGER,
  document_json  TEXT
);

CREATE TABLE loop_closures (
  closure_id        BLOB PRIMARY KEY,
  trajectory_id     BLOB NOT NULL,
  candidate_id      BLOB,
  source_frame_id   BLOB NOT NULL,
  target_frame_id   BLOB NOT NULL,
  status            TEXT,
  inlier_ratio      REAL,
  confidence        REAL,
  created_at_ns     INTEGER
);

CREATE TABLE optimization_results (
  result_id      BLOB PRIMARY KEY,
  graph_id       BLOB NOT NULL,
  trajectory_id  BLOB NOT NULL,
  status         TEXT,
  initial_error  REAL,
  final_error    REAL,
  created_at_ns  INTEGER,
  document_json  TEXT
);
```

---

## 15. Provenance / Lineage

### 15.1 Full Lineage Chain

```
Raw observations (ImageObservation)
    ↓  (feature extraction)
FeatureArtifacts
    ↓  (candidate generation — feature matching)
LoopClosureCandidate
    ↓  (geometric verification)
LoopClosure (accepted)
    ↓  (edge creation)
PoseGraph Edge
    ↓  (pose graph assembly)
PoseGraph
    ↓  (optimization)
OptimizationResult
    ↓  (trajectory correction)
Optimized Trajectory
    ↓  (reconstruction conversion)
Canonical Reconstruction v2
    ↓  (optional alignment)
T_world_camera
```

### 15.2 Provenance at Each Stage

| Stage | Provenance carrier | Key fields |
|-------|-------------------|------------|
| Feature extraction | FeatureArtifact manifest | input_artifact_hashes=[image_hash], configuration_hash |
| Candidate generation | LoopClosureCandidate.provenance | matcher, configuration_hash |
| Geometric verification | LoopClosure.provenance | verifier, inlier_threshold, configuration_hash |
| Pose graph assembly | PoseGraph.provenance | odometry_source, loop_closure_count |
| Optimization | OptimizationResult.provenance | optimizer, iterations, convergence_threshold |
| Reconstruction | ReconstructionProvenance (P2.5) | backend, trajectory_hash, optimization_hash |

### 15.3 Backward Reference

**D-PL-01:** Each artifact in the chain carries `input_artifact_hashes` pointing to its inputs. This creates an immutable DAG of provenance that can be traversed backwards from any reconstruction to its raw observations.

---

## 16. Uncertainty / Quality

### 16.1 Per-Pose Uncertainty

**D-UC-01:** Each pose node in the trajectory payload may carry:
- `covariance_position`: 6-element upper triangle of the 3×3 position covariance (xx, xy, xz, yy, yz, zz).
- `covariance_rotation`: 6-element upper triangle of the 3×3 rotation covariance (xx, xy, xz, yy, yz, zz).

When absent (zero-filled), the pose's uncertainty is unknown.

### 16.2 Optimization Effect on Uncertainty

**D-UC-02:** After optimization:
- Covariances are recomputed by the optimizer (if the optimizer supports it).
- Covariances typically **decrease** near loop closures (more constraints = more certainty).
- Covariances typically **increase** far from any loop closure (less constrained).
- The optimizer may not produce covariances (e.g. simple BA). In that case, covariances remain from the input trajectory.

### 16.3 Aggregate Uncertainty

```cpp
struct TrajectoryUncertainty {
  double mean_position_uncertainty_m;    // mean across all nodes
  double max_position_uncertainty_m;     // worst-case node
  double mean_rotation_uncertainty_rad;
  double max_rotation_uncertainty_rad;
  double loop_closure_density;           // closures per metre of trajectory
};
```

This is a summary for human-facing quality assessment. Detailed uncertainty is per-node in the payload.

### 16.4 Quality Integration with ADR-030

**D-UC-03:** Optimization quality metrics feed into the Quality Engine (ADR-030) as composable measures:
- `trajectory_error_reduction` — how much drift was corrected.
- `loop_closure_confidence` — aggregate confidence of all closures.
- `pose_graph_consistency` — fraction of edges with low residual after optimization.

---

## 17. Deterministic Identity

### 17.1 ID Generation Rules

| Entity | ID type | Deterministic? | Rationale |
|--------|---------|----------------|-----------|
| Trajectory | UUIDv4 | No | Instance identity — re-running produces new trajectory |
| PoseGraph | UUIDv4 | No | Instance identity |
| LoopClosureCandidate | UUIDv4 | No | Instance identity |
| LoopClosure | UUIDv4 | No | Instance identity |
| OptimizationResult | UUIDv4 | No | Instance identity |
| Pose payload nodes | integer (0, 1, 2, ...) | Yes | Ordered by sequence_index |
| PoseGraph nodes | integer (0, 1, 2, ...) | Yes | Ordered by sequence_index |
| PoseGraph edges | integer (0, 1, 2, ...) | Yes | Assigned during graph assembly |

### 17.2 Deterministic Re-Execution

**D-DI-01:** Re-running the same trajectory backend with the same inputs produces a **new trajectory** with new UUIDs. Deduplication is at the CAS level: if the pose payload bytes are identical, the CAS entry is deduplicated (ADR-010). The trajectory metadata row is always new.

**D-DI-02:** Re-running loop closure with the same inputs may produce different candidates (if the matcher is non-deterministic). Accepted closures should be deterministic given deterministic feature matching. This is verified by tests (AC-8).

---

## 18. Backend Adapter Boundary

### 18.1 SLAM Adapter Interface

A SLAM backend adapter implements `ProcessingAdapter` (existing interface) with these capabilities:

```
"trajectory_estimation"   → inputs: {feature, image}  → outputs: {trajectory}
"loop_closure"            → inputs: {trajectory, feature}  → outputs: {loop_closure, pose_graph}
"pose_graph_optimization" → inputs: {pose_graph, trajectory}  → outputs: {optimization_result, trajectory}
```

### 18.2 Adapter Boundaries

```
┌─────────────────────────────────────────────┐
│  Engine / Scheduler (backend-independent)    │
│    TaskRequest → CAS refs only               │
└──────────────┬──────────────────────────────┘
               │ Worker Protocol (protobuf frames)
┌──────────────▼──────────────────────────────┐
│  Worker Process                              │
│    ProcessingAdapter interface               │
│    ┌─────────────────────────────────────┐  │
│    │  SLAM Adapter                       │  │
│    │    Descriptor() → capabilities      │  │
│    │    CreatePlan() → plan steps        │  │
│    │    Execute() → CAS payloads         │  │
│    └─────────────────────────────────────┘  │
│    ┌─────────────────────────────────────┐  │
│    │  GTSAM Adapter (optimizer)          │  │
│    │    Optimizes PoseGraph              │  │
│    │    Produces OptimizationResult      │  │
│    │    Produces Optimized Trajectory    │  │
│    └─────────────────────────────────────┘  │
└─────────────────────────────────────────────┘
```

### 18.3 No Backend Types in Core

**D-AB-01:** GTSAM types (`NonlinearFactorGraph`, `Values`, `ISAM2Params`, etc.) never cross the adapter boundary. The adapter reads canonical `PoseGraph` and writes canonical `OptimizationResult` + `Trajectory`.

**D-AB-02:** SLAM-specific data structures (covisibility graph, bow database, keyframe DB) remain inside the SLAM adapter. The adapter exposes only canonical types.

---

## 19. Data-Flow Diagrams

### 19.1 SLAM Pipeline Flow

```
Images
  │
  ▼
Feature Extraction ──→ FeatureArtifacts
  │
  ▼
SLAM Backend (adapter)
  │
  ├──→ Trajectory (odometry poses)
  ├──→ FeatureArtifacts (if SLAM also extracts features)
  └──→ Reconstruction v2 (incremental, "reconstructing" status)
  │
  ▼
Loop Closure Detection (adapter)
  │
  ├──→ LoopClosureCandidates
  ├──→ LoopClosures (accepted/rejected)
  └──→ PoseGraph (with loop closure edges)
  │
  ▼
Pose Graph Optimization (adapter, e.g. GTSAM)
  │
  ├──→ OptimizationResult (metrics)
  └──→ Optimized Trajectory (corrected poses)
  │
  ▼
Reconstruction Update
  │
  └──→ New Reconstruction snapshot ("succeeded")
         ReconImage.pose = T_reconstruction_camera (from optimized trajectory)
```

### 19.2 SfM Pipeline Flow (Existing COLMAP Path)

```
Images
  │
  ▼
Feature Extraction ──→ FeatureArtifacts
  │
  ▼
COLMAP Adapter (existing, P2.5)
  │
  ├──→ SparseModel (COLMAP native)
  └──→ Reconstruction v2 ("succeeded")
         ReconImage.pose = T_reconstruction_camera (from COLMAP BA)
```

The SfM path does not use Trajectory/PoseGraph — COLMAP handles optimization internally. The trajectory concept is primarily for SLAM and long-sequence SfM.

---

## 20. State / Lifecycle Diagrams

### 20.1 Trajectory Lifecycle

```
         ┌──────────┐
         │ building  │ ← poses being added incrementally
         └────┬─────┘
              │  (all poses received)
              ▼
         ┌──────────┐
         │ optimized │ ← pose-graph optimization complete
         └────┬─────┘
              │  (superseded by newer trajectory)
              ▼
         ┌────────────┐
         │ superseded  │ ← replaced, but still queryable
         └────────────┘
```

### 20.2 PoseGraph Lifecycle

```
         ┌──────────┐
         │ building  │ ← edges being added
         └────┬─────┘
              │  (all edges added)
              ▼
         ┌──────────┐
         │   ready   │ ← submitted to optimizer
         └────┬─────┘
              │  (optimization started)
              ▼
         ┌────────────┐
         │ optimizing  │
         └──┬─────┬───┘
            │     │
   converged│     │failed/diverged
            ▼     ▼
     ┌──────────┐ ┌────────┐
     │optimized │ │ failed │
     └──────────┘ └────────┘
```

### 20.3 Loop Closure Lifecycle

```
Candidate Generated
       │
       ▼
Geometric Verification
       │
  ┌────┴────┐
  │         │
accepted  rejected
  │         │
  ▼         ▼
PoseGraph  (persisted for audit)
Edge
```

---

## 21. Normative Decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| D-TRJ-01 | Trajectory is a standalone CAS artifact with metadata row | Consistent with Reconstruction, FeatureArtifact patterns |
| D-TRJ-02 | Trajectory IDs are UUIDv4 (instance-scoped) | Re-running produces new trajectory; content dedup at CAS level |
| D-TRJ-03 | kind ∈ {odometry, slam, sfm, survey} | Covers all planned backend types |
| D-TRJ-04 | coordinate_frame follows D-CRM-06 naming | Consistent with existing convention |
| D-TRJ-05 | status ∈ {building, optimized, superseded} | Clear lifecycle |
| D-TRJ-06 | Aggregate uncertainty on entity, per-pose in payload | Lightweight metadata + detailed payload |
| D-TRJ-07 | Nodes ordered by sequence_index (gaps allowed) | Robust to dropped frames |
| D-TRJ-08 | Poses stored as T_trajectory_camera | Consistent with D-CRM-01 |
| D-TRJ-09 | Covariance optional per node | Not all backends produce covariances |
| D-TRJ-10 | Nodes reference Frame by frame_id (bridge) | No data duplication; existing bridge pattern |
| D-PG-01 | PoseGraph is a separate CAS artifact | Clean input/output contract for optimizer |
| D-PG-02 | PoseGraph lifecycle: building → ready → optimizing → optimized | Clear state machine |
| D-PG-03 | Graph nodes carry only id + frame_id + timestamp | No pose duplication; graph is connectivity only |
| D-PG-04 | Edge type ∈ {odometry, loop_closure, prior, gps, imu_preintegration, lidar_odometry} | Extensible vocabulary |
| D-PG-05 | Information matrix in translation-then-rotation ordering | ADR-007 |
| D-PG-06 | Confidence is informational; information matrix is mathematical | Separation of concerns |
| D-PG-07 | Edge provenance tracks source + configuration hash | Reproducibility |
| D-LC-01 | Loop closure is a semantic contract, not an algorithm | Backend independence |
| D-LC-02 | Candidates are persisted (not just accepted closures) | Debugging, audit, re-evaluation |
| D-LC-03 | Candidates persisted for audit trail | Quality assurance |
| D-LC-04 | Geometric verification checks inlier count, ratio, epipolar + spatial consistency | Standard criteria |
| D-LC-05 | Closures have status accepted/rejected | Both persisted |
| D-LC-06 | Temporal + spatial separation metadata on closures | Quality assessment, duplicate detection |
| D-LC-07 | Duplicate closures detected by (source, target) pair | Simple, effective |
| D-LC-08 | Near-duplicates rejected within configurable window | Prevents redundant edges |
| D-LC-09 | False-positive protection: geometric verification + thresholds + spatial consistency | Multi-layer defense |
| D-OPT-01 | Optimization is input(PoseGraph) → output(poses + metrics) | Clean contract |
| D-OPT-02 | Optimization status ∈ {converged, failed, diverged} | Clear outcomes |
| D-OPT-03 | Optimized poses stored as separate CAS document | Original preserved |
| D-OPT-04 | Post-optimization covariance from optimizer | When available |
| D-OPT-05 | Original trajectory never modified by optimization | Immutability |
| D-OPT-06 | Trajectory.status → "optimized" after optimization | Indicates optimized payload exists |
| D-CF-01 | Each trajectory defines its own coordinate frame | Decoupled from world frame |
| D-CF-02 | Trajectory poses are T_trajectory_camera | Consistent with D-CRM-01 |
| D-CF-03 | Alignment to project_world is a FrameGraph edge | Clean separation |
| D-CF-04 | Alignment may be identity, computed, or unknown | Handles all cases |
| D-CF-05 | T_reconstruction_camera = T_reconstruction_trajectory * T_trajectory_camera | Compositional |
| D-CF-06 | Trajectory frame remains separate from reconstruction frame | No silent frame merging |
| D-FI-01 | Trajectory references Frame by frame_id (bridge) | No embedding |
| D-FI-02 | Frame.pose_ref is optional, resolved by adapter | Convenience, not authoritative |
| D-RI-01 | Trajectory → Reconstruction follows COLMAP adapter pattern | Consistency |
| D-RI-02 | T_reconstruction_camera preserved, not converted to T_world_camera | D-CRM-01 upheld |
| D-RI-03 | ReconstructionProvenance records trajectory source + optimization hash | Full lineage |
| D-RI-04 | Each optimization produces new Reconstruction snapshot (D-CRM-19) | Versioning consistency |
| D-MS-01 | Each session has own trajectory | Session isolation |
| D-MS-02 | Multi-session reconstruction via session_ids (D-CRM-12) | Existing mechanism |
| D-MS-03 | Cross-session alignment via FrameGraph edge | Clean, compositional |
| D-MS-04 | Cross-session loop closure is future capability | Scope control |
| D-PL-01 | Provenance forms immutable DAG via input_artifact_hashes | Traceability |
| D-UC-01 | Per-pose covariance optional, 6-element upper triangle | ADR-007 ordering |
| D-UC-02 | Optimization changes covariance (typically reduces near closures) | Physical intuition |
| D-UC-03 | Quality metrics feed ADR-030 Quality Engine | Integration |
| D-DI-01 | Re-execution produces new trajectory; CAS deduplicates payloads | Idempotent storage |
| D-DI-02 | Closure generation should be deterministic given deterministic features | Testable |
| D-AB-01 | GTSAM types never cross adapter boundary | Adapter seam |
| D-AB-02 | SLAM-specific structures stay inside adapter | Backend independence |

---

## 22. Open Questions

| # | Question | Impact | Proposed resolution |
|---|----------|--------|---------------------|
| Q1 | Should PoseGraph be embedded in Trajectory or separate? | Data model complexity | **Separate** (D-PG-01). Rationale: different lifecycles, same graph may be re-optimized. |
| Q2 | Should loop-closure candidates be persisted or only accepted closures? | Storage, debugging | **Persist candidates** (D-LC-02). Rationale: audit trail, re-evaluation. |
| Q3 | How should multiple competing loop closures be represented? | Graph structure | **Each accepted closure is an independent edge.** Multiple edges between the same node pair are allowed (weighted by confidence). The optimizer handles over-constrained graphs. |
| Q4 | How should uncertainty propagate through optimization? | Mathematical correctness | **Optimizer computes posterior covariance when possible.** When not, input covariances are preserved. This is optimizer-dependent and cannot be mandated at the platform level. |
| Q5 | Should optimized poses coexist with original acquisition poses? | Data model | **Yes** — separate CAS documents (D-OPT-05). Original never destroyed. |
| Q6 | What is the canonical distinction between acquisition trajectory, optimized trajectory, and reconstruction camera poses? | Terminology, data model | **Acquisition** = raw odometry in trajectory local frame. **Optimized** = corrected poses in same trajectory local frame. **Reconstruction** = poses in reconstruction local frame via `T_reconstruction_trajectory * T_trajectory_camera`. Three distinct representations, all derivable from each other. |
| Q7 | How should a trajectory reference FeatureArtifacts without embedding feature data? | Data model | **Through Frame.** Trajectory → frame_id → Frame → FeatureSet → FeatureArtifact. No direct trajectory-to-feature reference needed. |
| Q8 | How should loop closure interact with Reconstruction v2 provenance? | Provenance | **ReconstructionProvenance records trajectory_id and optimization_result_id** in backend_specific_json or input_artifact_hashes. |
| Q9 | What is the boundary between loop closure, registration, multi-session alignment, and reconstruction alignment? | Scope, terminology | **Loop closure** = same-trajectory revisit detection. **Registration** = cross-trajectory or cross-session feature matching. **Alignment** = installing a FrameGraph edge between coordinate frames. **Reconstruction alignment** = computing `T_reconstruction_trajectory`. All are distinct pipeline stages. |
| Q10 | Should Frame.pose_ref be populated by the trajectory adapter? | Backward compatibility | **Optional, recommended.** Populating it enables quick trajectory lookup from Frame without loading the full trajectory. But it is not authoritative — the trajectory payload is. |

---

## 23. Implementation Implications

### 23.1 Storage Changes

New SQLite tables: `trajectories`, `pose_graphs`, `loop_closures`, `optimization_results` (see Section 14.3). Migration 0008 (or later, after P2.5 migration 0007).

### 23.2 New Core Types

New headers in `core/trajectory/`:
- `trajectory.h` — Trajectory entity
- `pose_graph.h` — PoseGraph entity
- `loop_closure.h` — LoopClosure, LoopClosureCandidate entities
- `optimization.h` — OptimizationResult entity

### 23.3 New CAS Schemas

New JSON schemas in `schemas/json/`:
- `trajectory.schema.json` — pose payload
- `pose_graph.schema.json` — graph payload
- `loop_closure.schema.json` — closure payload
- `optimization_result.schema.json` — result payload

### 23.4 New Adapter Capabilities

New capability strings in `worker-capabilities.schema.json`:
- `"trajectory_estimation"` — SLAM/odometry trajectory production
- `"loop_closure"` — loop closure detection and verification
- `"pose_graph_optimization"` — pose graph optimization (GTSAM adapter)

### 23.5 Engine/Worker Changes

**Minimal.** The existing `ProcessingAdapter` interface, `TaskRequest`, and `WorkerExecutor` are sufficient. New capabilities are registered in the pipeline registry. No protocol changes needed.

### 23.6 Query Extensions

`SceneQuery` gains:
- `TrajectoriesBySession(session_id)` — find trajectories for a session
- `LatestTrajectory(session_id)` — find the latest non-superseded trajectory
- `PoseGraphsByTrajectory(trajectory_id)` — find graphs for a trajectory
- `LoopClosuresByTrajectory(trajectory_id)` — find closures for a trajectory

---

## 24. Explicit Future Increments

| Increment | Description | Depends on |
|-----------|-------------|------------|
| P3-impl-1 | Core types + DB migration for Trajectory, PoseGraph, LoopClosure, OptimizationResult | P3 design |
| P3-impl-2 | JSON schemas for trajectory, pose_graph, loop_closure, optimization_result payloads | P3-impl-1 |
| P3-impl-3 | Trajectory adapter interface + COLMAP trajectory extraction (SfM path) | P3-impl-1, P3-impl-2 |
| P3-impl-4 | SLAM adapter interface + ORB-SLAM3 adapter (trajectory estimation) | P3-impl-1, P3-impl-3 |
| P3-impl-5 | Loop closure adapter interface + COLMAP matching adapter | P3-impl-1, P3-impl-3 |
| P3-impl-6 | GTSAM optimizer adapter | P3-impl-1, P3-impl-5 |
| P3-impl-7 | Trajectory → Reconstruction v2 integration (optimized poses → ReconImage.pose) | P3-impl-3, P3-impl-6 |
| P3-impl-8 | Multi-session alignment and cross-session registration | P3-impl-7 |
| P3-impl-9 | KISS-ICP LiDAR odometry adapter | P3-impl-1 |
| P3-impl-10 | Uncertainty propagation through optimization | P3-impl-6 |

---

*End of P3 design document.*
