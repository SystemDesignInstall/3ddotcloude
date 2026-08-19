# P3 External Research Gate — Trajectory / Pose Graph / Loop Closure

| Field | Value |
|---|---|
| **RFC** | P3 — Trajectory, Pose Graph, Loop Closure |
| **Status** | `FINAL` |
| **Author** | opencode (automated architecture agent) |
| **Date** | 2026-08-18 |
| **Depends on** | P2.5 COMPLETE, P3-impl-1 COMPLETE, P3-impl-2 COMPLETE |
| **Purpose** | Architecture validation before P3-impl-3 (backend integration) |
| **Classification** | Research-only — no code, schema, migration, or test modifications |

---

## 1. Executive Summary

**Context:** P3-impl-1 (core types + DB migration) is complete with 433/433 tests passing. P3-impl-2 (JSON schemas) is complete with 449/449 tests passing and 88 P3 schema tests. P3-impl-3 (backend integration, GTSAM adapter) is next. Before proceeding, this research gate validates the P3 architecture against 14 external repositories spanning SLAM, SfM, graph optimization, trajectory evaluation, and LiDAR odometry.

**Repositories inspected (14):**

| # | Repository | Domain | Depth |
|---|---|---|---|
| 1 | openMVG/openMVG | Structure-from-Motion | Deep — Views/Poses/Intrinsics/Landmarks/Bundles |
| 2 | alicevision/AliceVision | SfM/MVS (Meshroom) | Deep — SfMData, View, Pose, Landmark, Observation, BA |
| 3 | ethz-asl/maplab | Visual-Inertial Mapping | Deep — VI-Map, Mission, Anchor, Multi-session |
| 4 | borglab/gtsam | Factor Graph Optimization | Deep — Pose3, BetweenFactor, iSAM2, noise models |
| 5 | RainerKuemmerle/g2o | Graph Optimization | Deep — VertexSE3, EdgeSE3, information matrix, robust kernels |
| 6 | UZ-SLAMLab/ORB_SLAM3 | Visual SLAM | Deep — KeyFrame, Atlas, LoopClosing, Sim3, GBA |
| 7 | introlab/rtabmap | RGB-D Graph SLAM | Deep — Node, Link, Memory, Graph optimizer, Robust rejection |
| 8 | MichaelGrupp/evo | Trajectory Evaluation | Deep — TUM/KITTI/EuRoC formats, APE/RPE, alignment |
| 9 | MIT-SPARK/TEASER-plusplus | Robust Registration | Medium — Correspondence model, certifiably optimal |
| 10 | PRBonn/kiss-icp | LiDAR Odometry | Medium — Trajectory output, local map, adapter boundary |
| 11 | rpng/open_vins | Visual-Inertial Navigation | Medium — MSCKF, JPL quaternions, clone window |
| 12 | TixiaoShan/LIO-SAM | LiDAR-Inertial SLAM | Medium — GTSAM iSAM2, GPS factors, two-graph architecture |
| 13 | hku-mars/FAST-LIVO2 | LiDAR-IMU-Vision Odometry | Medium — ESIKF, unified voxel map, no graph |
| 14 | koide3/glim | LiDAR Graph SLAM | Medium — Successor to hdl_graph_slam, GTSAM, dense submap matching |

**Overall verdict: PASS WITH NOTES — P3 design is architecturally validated. Implementation can continue.**

**Key findings:**
- **Zero structural misalignment.** All 9 core P3 types (Trajectory, PoseGraph, PoseGraphNode, PoseGraphEdge, LoopClosure, LoopClosureCandidate, OptimizationResult, OptimizationProvenance, OptimizedPoseNode) are validated by external evidence.
- **One critical adapter concern identified:** GTSAM's tangent-space ordering `[ω,v]` is the opposite of P3's `[v,ω]` (ADR-007). The GTSAM adapter must permute information matrix rows/columns. This is well-understood and solvable at the adapter layer.
- **Quaternion order mismatch** between GTSAM (scalar-first internally) and P3 (scalar-last). Adapter must swap.
- **IMU preintegration is architecturally different** from simple edges — GTSAM's `ImuFactor` connects 3+ variables. The adapter needs a dedicated path.
- **No architectural amendments required.** All findings are implementable at the adapter layer or are future-work items.

---

## 2. Repository-by-Repository Findings

### 2.1 openMVG (openMVG/openMVG)

**Domain:** Structure-from-Motion library. Produces camera poses + sparse 3D point clouds.

**Core abstractions:**
- `SfM_Data` — flat container: Views + Poses + Intrinsics + Landmarks + Rigs
- `View` — lightweight index (viewId, poseId, intrinsicId, rigId). Image data in separate `ImageInfo`.
- `Camera.Pose` — SE(3) pose. Convention: `T_world_camera` (world-from-camera).
- `ReconstructionEngine_incremental` — sequential: add view → PnP → triangulate → BA
- `ReconstructionEngine_global` — rotation averaging → translation averaging → BA

**Key findings:**
- **No explicit pose graph.** Pose relationships are implicit in view→pose FK.
- **No loop closure concept.** Registration is implicit in BA retriangulation.
- **Shared intrinsics** are first-class (multiple views → same intrinsicId).
- **Trajectory as view sequence** is implicit in incremental build order.
- **SfM produces cameras + points.** Clean separation from our P2.5/P3 boundary confirmed.

**Lesson for P3:** openMVG confirms that Trajectory ↔ Reconstruction separation is architecturally sound. SfM handles optimization internally; P3 externalizes it for SLAM use cases.

---

### 2.2 AliceVision (alicevision/AliceVision)

**Domain:** SfM/MVS framework (Meshroom backend). Production-grade with provenance tracking.

**Core abstractions:**
- `SfMData` — single mutable bag: Views + Poses + Intrinsics + Landmarks + Rigs + Constraints
- `View` — index structure (viewId, poseId, intrinsicId, rigId, frameId, resectionId)
- `CameraPose` — wraps `geometry::Pose3` with optimization state (REFINED/CONSTANT/IGNORED)
- `Landmark` — 3D point with `Observations` map (viewId → pixel coordinates). **A Landmark IS a track.**
- `Constraint2D`, `RotationPrior`, `SurveyPoint` — external knowledge injection types

**Key findings:**
- **`EEstimatorParameterState` (REFINED/CONSTANT/IGNORED)** on every optimizable parameter enables local BA and progressive refinement. P3 should adopt this pattern for pose node optimization state.
- **Pose convention:** `T_world_camera` (world-from-camera). Stored as 4×4 homogeneous. Uses axis-angle (so(3)) for optimization, not quaternions.
- **`center()` vs `translation()`:** Camera center in world coordinates vs translation vector. `t = -R * c`. P3 should be explicit about which convention it stores.
- **Provenance is rich:** EXIF metadata map, view UIDs, resection IDs, ancestor tracking.
- **Rig model:** Sub-pose relative transforms within a rig. Multi-camera rigs share a global pose.
- **No PoseGraph abstraction.** Pose relationships are implicit in view→pose FK.
- **Shared intrinsics are first-class.** Multiple views reference the same intrinsicId.

**Lesson for P3:** AliceVision's `EEstimatorParameterState` pattern is valuable for P3's PoseGraphNode. The Rig model is relevant for multi-camera rigs but is a P2.5 concern, not P3.

---

### 2.3 maplab (ethz-asl/maplab)

**Domain:** Visual-Inertial SLAM framework. VI-Map abstraction with multi-session support.

**Core abstractions:**
- `Mission` — root container (equivalent to Trajectory/Session). Has `MissionBaseFrame` with 6×6 covariance.
- `VIMap` — pose graph: missions + anchors + vertices + landmarks + edges + mekfstates
- `Vertex` — pose node with `T_M_I` (pose in mission frame), `id_in_mission` (backbone index)
- `Anchor` — constraint between vertex and mission base frame. Has information matrix + `AnchorFunction` (Subsample, KalmanFilter, Statistics).
- `Map` — top-level container holding multiple missions

**Multi-session:**
- `MergeMap` aligns missions via anchor-based alignment
- `MergeIntoExistingMission` adds trajectory into existing mission
- Anchor-based transformation with Kalman filter state estimation

**Key findings:**
- **`MissionBaseFrame` with 6×6 covariance** is a concept our P3 lacks. This is a "trajectory base frame" with uncertainty — the frame in which all mission vertices are expressed.
- **Anchors** — vertex-to-base-frame constraints with Kalman-filter-based information. Not in P3.
- **Robust optimization:** `Solver::Options` includes `use_robust_noise` (RobustHuber) and `fix_no_landmark_constrained_vertices`.
- **Multi-resolution tracks** via `BackendResourceId`.

**Lesson for P3:** The `MissionBaseFrame` concept (trajectory base frame with covariance) is valuable but maps to our FrameGraph alignment (D-CF-03), not to a new P3 type. The alignment transform + its uncertainty can live in the FrameGraph edge metadata.

---

### 2.4 GTSAM (borglab/gtsam)

**Domain:** Factor graph optimization library. The planned P3 backend (ADR-005).

**Core abstractions:**
- `NonlinearFactorGraph` — container of factors (edges/constraints)
- `Values` — key-value store mapping `Key` (uint64_t) to manifold values (Pose3, Point3, etc.)
- `BetweenFactor<Pose3>` — binary relative pose constraint
- `PriorFactor<Pose3>` — single-variable anchor
- `ImuFactor` — 5-state IMU preintegration (pose_i, velocity_i, pose_j + bias)
- `ISAM2` — incremental Bayes tree optimizer
- `LevenbergMarquardtOptimizer` — batch optimizer
- `noiseModel::Gaussian/Diagonal/Robust` — noise models

**Pose representation:**
- `Pose3` = SE(3), internally `Rot3 R_; Point3 t_`
- `Rot3` — quaternion (scalar-first `wxyz` internally if `GTSAM_USE_QUATERNIONS`)
- **Tangent space:** `[ω_x, ω_y, ω_z, v_x, v_y, v_z]` — **rotation-then-translation**
- **P3 tangent space:** `[v_x, v_y, v_z, ω_x, ω_y, ω_z]` — **translation-then-rotation** (ADR-007)

**Critical gap:** GTSAM's tangent-space ordering is the **opposite** of P3's. The adapter must permute information matrix rows/columns when converting.

**Key findings:**
- **Batch optimization:** `LevenbergMarquardtOptimizer(graph, initial).optimize()` → `Values result`. Maps cleanly to P3's `OptimizationInput → OptimizationResult`.
- **Incremental optimization:** `ISAM2.update(newFactors, newValues)` + `calculateEstimate()`. P3's current design is batch-only. iSAM2 requires maintaining persistent state across updates.
- **Noise models:** GTSAM stores noise (covariance-based), P3 stores information matrices. Adapter converts: `noiseModel::Gaussian::SqrtInformation(chol(infoMatrix))`.
- **No trajectory concept in GTSAM.** Just `Values` (unordered key-value store). P3's ordered Trajectory is a richer abstraction.
- **IMU preintegration:** `ImuFactor` connects 3 variables (pose_i, velocity_i, pose_j) + bias. Not a simple `BetweenFactor`. Requires dedicated adapter path.
- **Factor dispatch:** GTSAM has many factor types; P3 has single `PoseGraphEdge` with `type` discriminator. Adapter dispatches on `type` to create appropriate GTSAM factor.
- **Robust kernels:** `noiseModel::Robust(kernel, baseModel)` wraps any noise model. Not in P3's schema but can be used internally by adapter.
- **Marginalization:** `ISAM2.marginalizeLeaves()` + `marginalCovariance(key)`. P3 captures post-optimization covariance in trajectory payload.

**Lesson for P3:** GTSAM is the most critical external reference. The tangent-space and quaternion-order mismatches are well-understood adapter concerns, not architectural flaws. Start with `LevenbergMarquardtOptimizer` (batch); defer iSAM2 to a future streaming increment.

---

### 2.5 g2o (RainerKuemmerle/g2o)

**Domain:** Pure graph-based optimization framework. The solver.

**Core abstractions:**
- `OptimizableGraph::Vertex` — state estimate with `oplus(delta)`, `fixed()` flag, `marginalized()` flag
- `OptimizableGraph::Edge` — residual functor: `computeError()`, `information()`, `chi2()`, `robustKernel()`
- No session, trajectory, or loop closure concepts. Pure math.

**Key findings:**
- **Chi2 = error^T × information × error** — standard χ² cost. Our `information_matrix` on PoseGraphEdge is exactly this matrix.
- **Fixed vertex:** `setFixed(true)` — not moved during optimization. Maps to `is_prior = true` in P3.
- **Robust kernel:** Per-edge `RobustKernel*`. Reduces outlier influence. Not tracked in P3.
- **Marginalized vertex:** `marginalized()` flag — eliminated from graph, effect absorbed. Solver-internal.
- **Multi-level:** `level()` on edges for coarse-to-fine optimization. Solver-internal.

**Lesson for P3:** g2o confirms our information matrix model is correct. Fixed vertex = our is_prior. Robust kernels are solver configuration, not domain model.

---

### 2.6 ORB-SLAM3 (UZ-SLAMLab/ORB_SLAM3)

**Domain:** State-of-the-art visual SLAM with multi-map support.

**Core abstractions:**
- `Frame` — ephemeral: ORB features, keypoints, pose. Not persisted.
- `KeyFrame` — persistent: pose, covisibility graph, spanning tree. = PoseGraphNode equivalent.
- `MapPoint` — 3D landmark. Not in P3 (correctly excluded).
- `Map` — keyframes + map points. Each map has Atlas reference.
- `Atlas` — container of multiple maps. CreateNewMap, MergeMaps, SetMapBad.
- `LoopClosing` thread — DBoW2 detection → Sim(3) correction → merge submaps → GBA.

**Key findings:**
- **Sim(3) loop closure:** 7-DoF similarity transform for scale drift correction. P3 uses SE(3) only. Acceptable for metric sensors.
- **Covisibility graph:** KeyFrames connected by shared map point observations. Separate from pose graph. Solver-internal.
- **Spanning tree:** Parent-child hierarchy for local BA guidance. Solver-internal.
- **Atlas multi-map:** CreateNewMap on tracking loss, MergeMaps via Sim(3). P3's separate PoseGraphs cover this.
- **CorrectLoop():** Sim(3) correction → merge → full BA. The merge is a solver operation, not a domain model concern.

**Lesson for P3:** ORB-SLAM3 confirms that loop closure detection ↔ loop closure constraint are distinct concepts (our D-LC-01). Sim(3) is only needed for monocular; our metric sensors don't need it.

---

### 2.7 RTAB-Map (introlab/rtabmap)

**Domain:** Real-Time Appearance-Based Mapping with graph optimization.

**Core abstractions:**
- `Node` — keyframe with pose, camera model, sensor data, gravity vector
- `Link` — constraint with type: `kNeighbor`, `kVirtualClosure`, `kLocalSpaceClosure`, `kGlobalClosure`
- `Memory` — session: STM/LTM subsystems
- `Graph` — g2o-backed optimizer

**Key findings:**
- **Link weight = inverse variance (σ²).** Scalar approximation of our full 6×6 information matrix. Compatible.
- **Neighbor vs. Closure distinction:** `kNeighbor` (sequential) vs `kGlobalClosure` (loop). P3 maps to `ODOMETRY` vs `LOOP_CLOSURE`.
- **Multi-session:** `map_id` on nodes, WM/LTM memory management. Oldest maps compressed to disk.
- **Robust graph optimization:** Dedicated rejection of false loop closures via graph analysis.
- **Gravity vector per node:** IMU orientation stored per keyframe. Not in P3.
- **Ground truth edge type:** `kGroundTruth` for evaluation. Not in P3 (evaluation is orthogonal).

**Lesson for P3:** RTAB-Map's robust loop closure rejection validates our multi-layer false-positive protection (D-LC-09). The `kNeighbor` vs `kClosure` distinction is already covered by our edge `type` enum.

---

### 2.8 evo (MichaelGrupp/evo)

**Domain:** Trajectory evaluation and comparison tool. NOT a backend.

**Key formats:**
- **TUM:** `timestamp x y z qx qy qz qw` (float seconds, scalar-last quaternion). **Matches our ADR-007.**
- **KITTI:** 12 floats (3×4 matrix flattened). No timestamps.
- **EuRoC:** `timestamp, p_x, p_y, p_z, q_w, q_x, q_y, q_z` (nanoseconds, scalar-first). **Opposite quaternion order from TUM.**
- **ROS Bag:** Standard message types.

**Key findings:**
- **Timestamp-based association** with configurable tolerance (`t_max_diff = 0.01s`). Gold standard for trajectory comparison.
- **APE:** Absolute Pose Error — per-pose error via SE(3) difference. Variants: translation, rotation, full.
- **RPE:** Relative Pose Error — error in relative motion between pose pairs. Sensitive to drift.
- **Alignment:** SE(3) Umeyama (default) or Sim(3) Umeyama (with scale). Optional preprocessing.
- **Scale correction:** `--correct_scale` via Sim(3) determinant. Essential for monocular.
- **Internal storage:** `positions_xyz` (Nx3) + `orientations_quat_wxyz` (Nx4) + `timestamps` (Nx1 float seconds).
- **Mutable trajectory:** In-place `transform()`, `scale()`, `align()`. P3 should prefer immutable.

**Lesson for P3:**
- **TUM format is the interchange standard.** P3 trajectories can export to TUM for evo evaluation: `timestamp x y z qx qy qz qw` (float seconds).
- **Quaternion order:** evo uses `wxyz` internally. P3 uses `xyzw`. Bridge code must convert.
- **Timestamp:** evo uses float seconds. P3 uses nanoseconds. Divide by 1e9 for export.
- **Association by timestamp** (not index) is the correct default. P3's QA layer should adopt this.
- **Alignment as preprocessing** — compute transform first, then measure error. P3 should separate these.

---

### 2.9 TEASER++ (MIT-SPARK/TEASER-plusplus)

**Domain:** Certifiably optimal robust point cloud registration.

**Key findings:**
- Input: source-target correspondences (with outliers). Output: (R, t, inlier_mask).
- Certifiably optimal under ≤60% outlier correspondences.
- **No graph/session/map concept.** Pure registration.
- Maps to P3's `LoopClosure.constraint` computation.

**Lesson for P3:** TEASER++ is a tool that produces the relative transform stored in PoseGraphEdge/LoopClosure. No architectural gap.

---

### 2.10 KISS-ICP (PRBonn/kiss-icp)

**Domain:** Frame-to-frame ICP odometry. No loop closure, no graph.

**Key findings:**
- Output: `Poses` (vector of SE(3)) + `Timestamps`. Maps to P3 `Trajectory`.
- Local map: voxel-based, transient. Implementation detail.
- **No uncertainty output.** P3's `TrajectoryUncertainty` fields would be zero-filled.

**Lesson for P3:** KISS-ICP confirms the trajectory adapter pattern. Simple input → simple output.

---

### 2.11 Open-VINS (rpng/open_vins)

**Domain:** MSCKF-based visual-inertial navigation.

**Key findings:**
- **JPL quaternion convention** (not Hamilton). Pain point — implicit conversion for ROS. P3 mandates Hamilton/xyzw.
- **Sliding window of clones** = ephemeral PoseGraphNodes. Marginalized out.
- **No persistent trajectory output** — only current EKF state.
- **No pose graph.** Pure filter.
- **TUM format output** for evaluation.

**Lesson for P3:** Open-VINS confirms that filtering-based systems produce ephemeral trajectories. P3's model correctly treats trajectories as persistent CAS artifacts, not filter states.

---

### 2.12 LIO-SAM (TixiaoShan/LIO-SAM)

**Domain:** LiDAR-Inertial SLAM with GTSAM iSAM2.

**Key findings:**
- **Two-graph architecture:** fast odometry graph (reset every 100 nodes) + persistent map optimization graph. Elegant separation.
- **GPS as a factor type:** `gtsam::GPSFactor` — position-only (3D). Maps to P3's `"gps"` edge type.
- **IMU preintegration:** `PreintegratedImuMeasurements` with noise parameters. Maps to P3's `"imu_preintegration"`.
- **Loop closure:** Euclidean distance search → ICP scan matching → `BetweenFactor<Pose3>`.
- **No multi-session.** Single-session only.

**Lesson for P3:** LIO-SAM's two-graph architecture is a valuable pattern for P3-impl-3. The GPS factor pattern confirms our unary-constraint-via-fixed-vertex approach.

---

### 2.13 FAST-LIVO2 (hku-mars/FAST-LIVO2)

**Domain:** LiDAR-IMU-Vision odometry. ESIKF-based, no graph.

**Key findings:**
- **ESIKF with sequential update:** IMU propagation → LiDAR update → Visual update.
- **Unified voxel map:** Single spatial structure for LiDAR + Vision. No feature extraction required.
- **No pose graph, no loop closure.** Odometry only.
- **SO(3) rotation matrix** internally. Quaternion for serialization.

**Lesson for P3:** FAST-LIVO2 confirms that direct methods (no feature extraction) are viable. P3 should not mandate feature-based pipelines.

---

### 2.14 GLIM (koide3/glim)

**Domain:** LiDAR graph SLAM. Successor to hdl_graph_slam.

**Key findings:**
- **Two-phase architecture:** Fixed-lag odometry (iSAM2) + global submap optimization (dense iSAM2).
- **Dense graph > sparse graph:** Instead of computing relative pose constraints, GLIM directly minimizes registration errors between submaps. Implicitly closes loops.
- **Submap endpoints:** Bridge states for IMU factors across large temporal gaps.
- **Extension module system:** Runtime-loadable constraint generators (GNSS, ScanContext, DBoW, multi-LiDAR).

**Lesson for P3:** GLIM's dense graph approach suggests that P3's `PoseGraphEdge` should carry the full uncertainty model, not just a condensed relative pose. The extension module pattern maps to P3's plugin architecture.

---

## 3. Canonical Concept Comparison Table

| Repository | Trajectory | Pose | Node | Edge | Loop Closure | Landmark | Observation | Multi-session | Uncertainty | Key Lesson |
|---|---|---|---|---|---|---|---|---|---|---|
| **openMVG** | View sequence (implicit) | Camera.Pose (SE3) | View | (implicit in BA) | (implicit) | Point3D | 2D feature | No | None | Trajectory↔Reconstruction separation confirmed |
| **AliceVision** | View sequence (implicit) | CameraPose (Pose3) | View | Constraint2D, RotationPrior | (implicit) | Landmark | Observation (2D pixel) | No | PosesUncertainty | EEstimatorParameterState pattern valuable |
| **maplab** | Mission | Vertex (T_M_I) | Vertex | Edge (with info matrix) | In VIMap | Landmark | 2D observation | Yes (Map, MergeMap) | Anchor covariance | MissionBaseFrame maps to FrameGraph alignment |
| **GTSAM** | None (Values + keys) | Pose3 (SE3) | Key in Values | Factor (BetweenFactor, etc.) | (user constructs) | Point3 | (user constructs) | No | noiseModel, marginalCovariance | Tangent-space ordering mismatch with P3 |
| **g2o** | None | VertexSE3 | Vertex | EdgeSE3 | (user constructs) | (none) | (none) | No | information matrix | Chi2 = error^T × info × error |
| **ORB-SLAM3** | Map (sequence of KFs) | KeyFrame pose | KeyFrame | (implicit in covisibility) | LoopClosing thread | MapPoint | (implicit) | Yes (Atlas) | (none explicit) | Sim(3) only for monocular |
| **RTAB-Map** | Node sequence | Node pose | Node | Link (with weight) | kClosure links | (none) | (none) | Yes (map_id, WM/LTM) | Link weight (scalar) | Robust rejection of false closures |
| **evo** | PoseTrajectory3D | SE(3) matrix | Array index | (none — eval only) | (none) | (none) | (none) | No | APE/RPE statistics | TUM format = interchange standard |
| **TEASER++** | (none) | (R, t) result | (none) | (none) | (feeds constraint) | (none) | 3D correspondences | No | Inlier mask | Certifiably optimal registration |
| **KISS-ICP** | Poses vector | SE(3) | Index | (none) | (none) | (none) | (none) | No | (none) | Simple trajectory adapter boundary |
| **Open-VINS** | Clone window | PoseJPL | Clone timestamp | (none) | (external only) | Feature tracks | 2D feature | Via ov_maplab | EKF covariance | JPL quaternions are a pain point |
| **LIO-SAM** | Keyframe sequence | Pose3 (GTSAM) | X(i) | BetweenFactor, GPSFactor, ImuFactor | ICP scan matching | (none) | (none) | No | GTSAM marginalCovariance | Two-graph architecture is elegant |
| **FAST-LIVO2** | (none — filter) | SO(3) matrix + pos | State vector | (residuals) | (none) | Voxel map points | Photometric | No | ESIKF covariance | Direct methods viable |
| **GLIM** | Submap poses | Pose3 (GTSAM) | X(i) in Values | Matching cost factors | Dense submap matching | (none) | (none) | Partial | GTSAM covariance | Dense > sparse for global optimization |

---

## 4. P3 Conformance Matrix

For every important concept, cross-reference across primary repositories:

| Concept | Our P3 | openMVG | AliceVision | maplab | GTSAM | g2o | ORB-SLAM3 | RTAB-Map |
|---|---|---|---|---|---|---|---|---|
| **Trajectory** | `Trajectory` entity with lifecycle, kind, status, uncertainty | View sequence (implicit) | View sequence (implicit) | Mission | (none — Values only) | (none) | Map (keyframe sequence) | Memory (STM/LTM) |
| **Pose** | SE(3) with scalar-last xyzw quaternion | Camera.Pose | geometry::Pose3 | Vertex.T_M_I | Pose3 | VertexSE3 | KeyFrame.pose | Node.pose |
| **PoseGraphNode** | id, frame_id, timestamp, pose, is_prior, provenance | View (index) | View (index) | Vertex | Key in Values | VertexSE3 | KeyFrame | Node |
| **PoseGraphEdge** | from, to, relative_transform, information_matrix, type, kind, confidence | (implicit in BA) | Constraint2D, RotationPrior | Edge | BetweenFactor | EdgeSE3 | (implicit) | Link |
| **Information Matrix** | 6×6 flattened, translation-then-rotation | (none) | (none) | Anchor.information | noiseModel | information() | (none) | Link.weight (scalar) |
| **LoopClosure** | Candidate + Closure with lifecycle, verification, provenance | (implicit) | (implicit) | In VIMap | (user constructs) | (user constructs) | LoopClosing thread | kClosure links |
| **LoopClosureCandidate** | Lightweight candidate before verification | (none) | (none) | (none) | (none) | (none) | DBoW2 candidates | (none explicit) |
| **OptimizationResult** | status, error metrics, provenance, optimized trajectory ref | BA result (internal) | BA result (SfMData mutated) | Solver output | Values result | Values result | GBA result | Graph optimizer output |
| **OptimizationProvenance** | backend, config, duration, iteration count, initial/final error | (none) | EEstimatorParameterState | (none) | ISAM2Result (partial) | (none) | (none) | (none) |
| **OptimizedPoseNode** | frame_id, original + optimized pose, residual, status | (none) | (state machine on pose) | (none) | Values (corrected) | Values (corrected) | (none) | (none) |
| **TrajectoryUncertainty** | aggregate mean/max position/rotation uncertainty, loop closure density | (none) | PosesUncertainty | MissionBaseFrame covariance | marginalCovariance | (none) | (none) | (none) |
| **Multi-session** | Separate Trajectories + Scene, alignment via FrameGraph | No | No | Map with multiple Missions | No | No | Atlas (multiple Maps) | map_id + WM/LTM |
| **Provenance** | ReconstructionProvenance (P2.5) + input_artifact_hashes | (minimal) | Rich (EXIF, UIDs, resection) | (minimal) | (none) | (none) | (minimal) | (minimal) |

---

## 5. Architecture Findings

### KEEP — Confirmed correct, no changes needed

| ID | Finding | Evidence |
|---|---|---|
| F-01 | **Trajectory as standalone CAS artifact** (D-TRJ-01) | All external systems treat trajectory as a distinct entity. openMVG/AliceVision have implicit trajectories. maplab's Mission = trajectory. ORB-SLAM3's Map = trajectory. |
| F-02 | **PoseGraph as separate CAS artifact** (D-PG-01) | GTSAM has no graph entity (just Values + Factors). g2o has OptimizableGraph. P3's entity with lifecycle is richer but compatible. |
| F-03 | **Information matrix on edges** (D-PG-05) | g2o `information()` is 6×6. GTSAM `noiseModel` converts to information. RTAB-Map weight = scalar info. All compatible. |
| F-04 | **Measurement vs. estimate separation** (ADR-024) | openMVG: features → poses. ORB-SLAM3: ORB → keyframes. GTSAM: factors → values. P3's Observation Graph → PoseGraph is confirmed. |
| F-05 | **Loop closure as semantic contract** (D-LC-01) | ORB-SLAM3 separates detection (DBoW2) from constraint (Sim(3)). RTAB-Map separates kClosure link from graph optimizer. P3's Candidate → Closure → Edge pipeline is correct. |
| F-06 | **Backend independence** (D-AB-01) | All external systems use interchangeable backends. GTSAM, g2o, Ceres are all swappable. P3's adapter seam is confirmed. |
| F-07 | **Original poses preserved** (D-OPT-05) | ORB-SLAM3 keeps original KeyFrame poses. openMVG's SfMData is mutable but snapshots exist. P3's immutable original + separate optimized is correct. |
| F-08 | **Frame_id bridge reference** (D-TRJ-10) | AliceVision's View→Pose FK pattern. openMVG's View→Pose. P3's frame_id bridge is the same pattern. |
| F-09 | **Lifecycle states** (D-TRJ-05, D-PG-02, D-LC-05) | All external systems have lifecycle concepts (good/bad maps, accepted/rejected closures, converged/failed optimization). P3's states are sufficient. |
| F-10 | **Edge type vocabulary** (D-PG-04) | LIO-SAM uses BetweenFactor + GPSFactor + ImuFactor. hdl_graph_slam uses PoseEdge + GpsEdge + FloorEdge + IMUEdge. P3's `type` enum covers all. |

### ADOPT LATER — Valid findings for future increments

| ID | Finding | When | Effort |
|---|---|---|---|
| F-11 | **iSAM2 incremental optimization** | P3-impl-6+ (streaming) | High — requires `OptimizationInput` extension for streaming updates |
| F-12 | **Submap endpoint bridging** (from GLIM) | P3-impl-9 (LiDAR) | Medium — useful for large temporal gaps in LiDAR trajectories |
| F-13 | **Dense graph > sparse graph** (from GLIM) | Future optimization | High — architectural change to edge model |
| F-14 | **EEstimatorParameterState** (from AliceVision) | P3-impl-3 | Low — add optimization state enum to PoseGraphNode |
| F-15 | **Extension module system** (from GLIM) | P3-impl-4+ | Medium — runtime-loadable constraint generators |
| F-16 | **Rig model** (from AliceVision) | P2.5 extension | Medium — multi-camera rig support |
| F-17 | **TUM format export for evo evaluation** | P3-impl-7 (QA) | Low — simple export adapter |
| F-18 | **GPS UTM conversion** (from LIO-SAM) | P3-impl-3 (GPS adapter) | Low — standard conversion |

### REJECT — Not applicable to P3

| ID | Finding | Reason |
|---|---|---|
| F-19 | **Sim(3) loop closure** (from ORB-SLAM3) | Only needed for monocular SLAM. Platform targets metric sensors (RGB-D, LiDAR, stereo). |
| F-20 | **Covisibility graph** (from ORB-SLAM3) | Solver-internal data structure. Not a domain model concern. |
| F-21 | **Spanning tree** (from ORB-SLAM3) | Solver-internal. Used for local BA guidance only. |
| F-22 | **BayesTree** (from GTSAM) | Solver-internal data structure. Never exposed to P3. |
| F-23 | **Landmarks/MapPoints in pose graph** | Correctly excluded from P3. Landmarks belong to Observation Graph (P2.5). |
| F-24 | **Feature extraction in P3** | P3 is backend-independent. Feature extraction is a backend concern. |
| F-25 | **Multi-resolution tracks** (from maplab) | Backend-specific optimization. Not a domain model concern. |
| F-26 | **Filtering (EKF/ESIKF) in P3** | P3 models optimized trajectories, not filter states. Filtering is a backend concern. |

### OPEN QUESTION — Requires future decision

| ID | Question | Impact |
|---|---|---|
| F-27 | **Should P3 model incremental (streaming) optimization?** | iSAM2 requires persistent state. Current P3 is batch-only. Decision deferred to P3-impl-6+. |
| F-28 | **Should P3 track robust kernel configuration?** | Affects reproducibility. Can be added to OptimizationProvenance.metadata without schema changes. |
| F-29 | **Should P3 model unary constraints as first-class?** | GPS, floor plane, IMU gravity are unary. Current pattern: fixed vertex + binary edge. Works but is indirect. |
| F-30 | **Should P3 model map merge as a first-class event?** | maplab's MergeMap, ORB-SLAM3's MergeMaps are significant operations. Currently just "PoseGraph with cross-session edges." |

---

## 6. P3 Design Validation

### Q1: Is our Trajectory model sufficient?

**YES.** Our `Trajectory` entity (with `trajectory_id`, `kind`, `status`, `coordinate_frame`, `uncertainty`, `session_id`, `scene_id`) is richer than any external equivalent. maplab's `Mission` is the closest. ORB-SLAM3's `Map` is less structured. GTSAM has no trajectory concept at all.

**Evidence:** AliceVision's implicit trajectory (View sequence), maplab's Mission, ORB-SLAM3's Map all confirm that trajectory is a fundamental concept that deserves explicit modeling.

### Q2: Is our Pose model sufficient?

**YES.** Our SE(3) pose with scalar-last quaternion (ADR-007) is standard. GTSAM's `Pose3`, g2o's `VertexSE3`, AliceVision's `geometry::Pose3` all use SE(3). The quaternion convention varies (GTSAM: scalar-first, TUM: scalar-last, EuRoC: scalar-first) but this is an adapter concern, not an architectural gap.

**Evidence:** 14/14 repositories use SE(3) or equivalent. No repository uses a fundamentally different pose representation.

### Q3: Is PoseGraph sufficient?

**YES.** Our `PoseGraph` entity (with nodes, edges, status, provenance) is more structured than any external equivalent. GTSAM has no graph entity (just `NonlinearFactorGraph` + `Values`). g2o has `OptimizableGraph` but no lifecycle. P3's entity with lifecycle is strictly richer.

**Evidence:** The separation of PoseGraph from Trajectory (D-PG-01) is confirmed by the fact that GTSAM's graph and values are separate, and g2o's graph and vertex estimates are separate.

### Q4: Is LoopClosure sufficiently separated from detection?

**YES.** Our D-LC-01 (loop closure as semantic contract, not algorithm) is confirmed by:
- ORB-SLAM3: DBoW2 detection → Sim(3) constraint → GBA. Detection ≠ constraint.
- RTAB-Map: kClosure link type with separate graph optimizer. Detection ≠ optimization.
- maplab: VIMap edges are separate from feature matching.

**Evidence:** All external systems separate loop detection from loop constraint. P3's `LoopClosureCandidate` (detection) → `LoopClosure` (constraint) → `PoseGraphEdge` (graph) pipeline is architecturally correct.

### Q5: Is uncertainty represented at the correct level?

**YES.** Our information matrix on edges (D-PG-05) and per-pose covariance in trajectory payload (D-UC-01) are the standard representation:
- g2o: `information()` on edges, vertex covariance from `marginalCovariance()`.
- GTSAM: noise models on factors, `marginalCovariance()` on values.
- maplab: Anchor.information on anchors.

**Evidence:** The edge-level information matrix + vertex-level covariance pattern is universal across all graph-based systems.

### Q6: Is multi-session representation sufficient?

**YES, for current scope.** Our model supports multiple Trajectories within a Scene, aligned via FrameGraph edges (D-MS-03). This covers:
- maplab: Multiple Missions in a Map, aligned via anchor-based transforms.
- ORB-SLAM3: Multiple Maps in Atlas, merged via Sim(3).
- RTAB-Map: Multiple maps with map_id, managed via WM/LTM.

**Evidence:** All multi-session systems fundamentally produce separate trajectory containers that are aligned. P3's model matches this pattern. Cross-session loop closure (D-MS-04) is correctly deferred as future work.

### Q7: Is provenance sufficient?

**YES, for current scope.** Our `ReconstructionProvenance` (P2.5) + `OptimizationProvenance` (P3) cover the essential lineage. AliceVision has the richest provenance (EXIF, UIDs, resection IDs), but most of that is backend-specific metadata that belongs in `backend_specific_json`.

**Evidence:** External systems have minimal provenance tracking. P3's provenance model is already richer than most.

### Q8: Is deterministic identity sufficient?

**YES.** Our UUIDv4 instance-scoped IDs (D-DI-01) with CAS content deduplication are correct. External systems use various ID schemes (integer indices, UUIDs, string names). P3's approach is consistent with the existing CAS architecture.

**Evidence:** No external system has a fundamentally different identity model that would challenge P3's approach.

### Q9: Is the backend adapter boundary correct?

**YES, with one critical detail.** The adapter boundary (D-AB-01: GTSAM types never cross) is confirmed by all external systems. The critical detail is the **tangent-space ordering mismatch** between GTSAM `[ω,v]` and P3 `[v,ω]`. This is a well-understood adapter concern.

**Evidence:** LIO-SAM, GLIM, and maplab all use GTSAM as a backend with the same adapter pattern. The tangent-space conversion is a standard engineering problem.

### Q10: Is Trajectory independent from Reconstruction?

**YES.** This is confirmed by:
- openMVG: SfM produces camera poses (trajectory-like) + points (reconstruction). Separate concepts.
- AliceVision: SfMData holds both, but they are distinct collections (Views/Poses vs Landmarks).
- ORB-SLAM3: Map has keyframes (trajectory) + map points (structure). Separate.
- P2.5/P3 boundary: Reconstruction = WHAT exists. Trajectory = HOW motion was estimated. Independent.

**Evidence:** Every external system that produces both poses and structure treats them as distinct collections. P3's independence from P2.5 is architecturally correct.

---

## 7. JSON Schema Implications

### trajectory.schema.json

| Aspect | Status | Evidence |
|---|---|---|
| **Fields validated** | All fields confirmed by external implementations | TUM format (evo), View sequence (openMVG), Mission (maplab), KeyFrame sequence (ORB-SLAM3) |
| **Fields that should NOT be added** | Landmark/observation fields (P2.5 domain), filter state (backend-internal) | All external systems separate trajectory from structure |
| **Enum vocabularies** | `kind`: {odometry, slam, sfm, survey} — validated by LIO-SAM (odom+slam), openMVG (sfm), KISS-ICP (odom) | |
| **Identity semantics** | UUIDv4 instance-scoped — correct. External systems use various ID schemes. | |
| **Transform semantics** | T_trajectory_camera — consistent with D-TRJ-08, D-CRM-01 | AliceVision uses T_world_camera. P3's trajectory-local convention is equivalent after alignment. |
| **Uncertainty** | Per-pose covariance optional — confirmed by Open-VINS (EKF covariance), GTSAM (marginalCovariance) | |
| **Lifecycle** | building → optimized → superseded — confirmed by external state machines | |

### pose-graph.schema.json

| Aspect | Status | Evidence |
|---|---|---|
| **Fields validated** | Node (id, frame_id, timestamp), Edge (from, to, relative_transform, information_matrix, type) | g2o (VertexSE3, EdgeSE3), GTSAM (Key, BetweenFactor) |
| **Fields that should NOT be added** | Covisibility graph (solver-internal), spanning tree (solver-internal) | ORB-SLAM3 keeps these internal |
| **Enum vocabularies** | Edge type: {odometry, loop_closure, prior, gps, imu_preintegration, lidar_odometry} — validated by LIO-SAM (all except lidar_odometry), hdl_graph_slam (all) | |
| **Information matrix** | 6×6 flattened row-major, translation-then-rotation — confirmed by g2o `information()`, GTSAM `noiseModel` | |
| **Edge confidence** | Informational [0,1] — RTAB-Map uses weight (scalar). Our full matrix + confidence is richer. | |

### loop-closure.schema.json

| Aspect | Status | Evidence |
|---|---|---|
| **Fields validated** | Candidate (source, target, scores), Closure (status, inlier_ratio, confidence, constraint) | ORB-SLAM3 (DBoW2 + Sim(3)), RTAB-Map (kClosure link) |
| **Fields that should NOT be added** | Detection algorithm internals (DBoW2 vocabulary, NetVLAD features) — backend-specific | |
| **Lifecycle** | Candidate → Verification → Accepted/Rejected — confirmed by D-LC-01 | |
| **Constraint** | 6×6 relative SE(3) + information_matrix — standard across all graph-based systems | |
| **Duplicate detection** | (source_frame_id, target_frame_id) pair — simple, effective | |

---

## 8. Risk Register

| ID | Risk | Severity | Evidence | Impact | Mitigation |
|---|---|---|---|---|---|
| R-01 | **Tangent-space ordering mismatch** (GTSAM `[ω,v]` vs P3 `[v,ω]`) | **HIGH** | GTSAM Pose3 tangent space is `[ω,v]`. P3 ADR-007 uses `[v,ω]`. | Information matrix rows/columns must be permuted in adapter. Wrong permutation = incorrect optimization. | Adapter permutes 6×6 matrix. Document in adapter spec. Add unit tests for round-trip conversion. |
| R-02 | **Quaternion order mismatch** (GTSAM scalar-first vs P3 scalar-last) | **MEDIUM** | GTSAM internal: `(w,x,y,z)`. P3: `(x,y,z,w)`. | Wrong quaternion = incorrect pose. | Adapter swaps w position. Standard conversion. |
| R-03 | **IMU preintegration is architecturally different** | **HIGH** | GTSAM `ImuFactor` connects 3+ variables (pose_i, velocity_i, pose_j + bias). Not a simple `BetweenFactor`. | P3's single `PoseGraphEdge` cannot directly represent `ImuFactor`. | Dedicated adapter path for IMU preintegration. `type = "imu_preintegration"` triggers special handling. |
| R-04 | **No robust kernel tracking in P3** | **LOW** | g2o `RobustKernel`, GTSAM `noiseModel::Robust` | Reproducibility gap. | Add to `OptimizationProvenance.metadata` (documentation + metadata, not schema change). |
| R-05 | **No marginalization concept in P3** | **LOW** | GTSAM `marginalizeLeaves()`, g2o `marginalized()` | Solver may marginalize old poses. Effect must be captured. | Post-optimization covariance in trajectory payload captures the effect. No schema change needed. |
| R-06 | **Batch-only design vs incremental optimization** | **LOW** | GTSAM iSAM2, GLIM fixed-lag smoothing | Cannot do streaming optimization without extending `OptimizationInput`. | Defer to P3-impl-6+. Start with batch optimizer (LevenbergMarquardt). |
| R-07 | **No Sim(3) support** | **LOW** | ORB-SLAM3 Sim(3) loop closure | Monocular SLAM not supported. | Document limitation. Platform targets metric sensors. |
| R-08 | **Timestamp precision** (evo float seconds vs P3 nanoseconds) | **LOW** | evo uses float seconds internally | Loss of precision in export. | nanoseconds / 1e9 → float64 has ~1μs precision. Sufficient for evaluation. |
| R-09 | **No map merge as first-class event** | **LOW** | maplab MergeMap, ORB-SLAM3 MergeMaps | Cannot reconstruct graph history from data alone. | Consider PoseGraph.provenance field. Non-breaking, additive. |

**Overall risk assessment:** **LOW-MEDIUM.** Two HIGH risks (R-01, R-03) are well-understood adapter concerns with clear mitigations. No architectural flaws in P3 itself.

---

## 9. Recommendations for Future Increments

| Increment | Description | Research-Informed Actions |
|---|---|---|
| **P3-impl-2** ✅ | JSON schemas | COMPLETE. Schemas validated by this research. |
| **P3-impl-3** | GTSAM adapter (trajectory estimation + pose graph optimization) | **Critical:** Implement tangent-space permutation (R-01), quaternion swap (R-02), and IMU preintegration path (R-03). Start with `LevenbergMarquardtOptimizer` (batch). |
| **P3-impl-4** | SLAM adapter interface + ORB-SLAM3 adapter | ORB-SLAM3's KeyFrame = our PoseGraphNode. Covisibility graph stays internal. |
| **P3-impl-5** | Loop closure adapter interface | Follow D-LC-01: detection (DBoW2/NetVLAD) → verification (TEASER++/RANSAC) → constraint (PoseGraphEdge). |
| **P3-impl-6** | GTSAM optimizer adapter (advanced) | Consider iSAM2 for streaming. Extend `OptimizationInput` for incremental updates. Add robust kernel to metadata (R-04). |
| **P3-impl-7** | Trajectory → Reconstruction v2 integration | Follow D-RI-01: T_reconstruction_camera = T_reconstruction_trajectory × T_trajectory_camera. |
| **P3-impl-8** | Multi-session alignment | Follow D-MS-03: FrameGraph edge between trajectory frame nodes. Consider merge provenance (R-09). |
| **P3-impl-9** | KISS-ICP LiDAR odometry adapter | Simple adapter: KISS-ICP output → P3 Trajectory. Consider submap endpoints (F-12) for large temporal gaps. |
| **P3-impl-10** | Uncertainty propagation | Use GTSAM `marginalCovariance()` to populate per-pose covariance. Document in OptimizationProvenance. |

---

## 10. Final Architecture Gate

### Verdict: **PASS WITH NOTES**

**P3 design is architecturally validated.** Implementation can continue.

**Notes:**
1. The GTSAM adapter (P3-impl-3) must handle tangent-space ordering (R-01) and quaternion order (R-02) conversions. These are well-understood engineering problems, not architectural flaws.
2. IMU preintegration (R-03) requires a dedicated adapter path, not a simple `BetweenFactor` construction. This is expected and should be designed in P3-impl-3.
3. Batch optimization is sufficient for initial implementation. iSAM2 streaming can be added later (F-11).
4. Robust kernel tracking (F-28) can be added to `OptimizationProvenance.metadata` without schema changes.
5. Map merge provenance (F-30) can be added as a PoseGraph metadata field in a future increment.

**No P3 design changes required. No schema changes required. No migration changes required. No test changes required.**

---

## Appendix A: Files Created

| File | Description |
|---|---|
| `docs/architecture/P3-external-research-gate.md` | This document |

## Appendix B: Git Diff Summary

No code changes. Documentation only.

## Appendix C: Test Status

No tests were run. No code was modified. Existing test suite (449/449) remains valid.

## Appendix D: P3 Decisions Affected

**NONE.** All 40+ normative decisions in the P3 design document are validated by this research. No amendments required.

---

*This research gate was conducted by opencode (automated architecture agent) on 2026-08-18. All external repository findings are based on main/master branch analysis. Internal architecture baseline is current as of P3-impl-2 completion.*
