# P3-impl-6 Implementation Mapping — GTSAM Optimizer Adapter

**Status:** Pre-code architectural gate — mapping only  
**Date:** 2026-08-19  
**Author:** opencode  
**Depends on:** P3-impl-1 ✅, P3-impl-2 ✅, P3-impl-3 ✅, P3 External Research Gate ✅, GTSAM Design Gate ✅  
**Scope:** Mapping document only — no code, schema, dependency, or build changes

---

## 1. Current Repository State

### 1.1 GTSAM Absence

**GTSAM is NOT a repository dependency.** Verified by:

- `grep -r gtsam CMakeLists.txt` → 0 matches
- `grep -r gtsam conanfile.txt` → 0 matches
- `grep -r gtsam conan.lock` → 0 matches
- `grep -rGTSAM` across all `.h` files → 0 matches (only comments in `pose_graph.h` and `optimization.h` mentioning "GTSAM" as a future backend name)
- No `adapters/gtsam/` directory exists
- No GTSAM headers included anywhere

### 1.2 Dependency Management

**Mechanism:** Conan 2 with CMakeDeps generator (ADR-003).

**Current dependencies** (`conanfile.txt`):
```
eigen/[>=3.4.0 <3.5.0]
sqlite3/[>=3.47.0 <3.51.0]
nlohmann_json/[>=3.11.0 <3.13.0]
gtest/[>=1.15.0 <2.0.0]
protobuf/[>=5.29.0 <6.0.0]
```

**CMake integration** (root `CMakeLists.txt`):
```cmake
find_package(Eigen3 CONFIG REQUIRED)
find_package(SQLite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
```

### 1.3 Where GTSAM Should Be Introduced

**In `conanfile.txt`** under `[requires]`:
```
gtsam/[>=4.2.0 <5.0.0]
```

**In root `CMakeLists.txt`** (conditional, adapter-only):
```cmake
find_package(GTSAM CONFIG)
if(GTSAM_FOUND)
    set(SPATIAL_HAS_GTSAM ON)
else()
    set(SPATIAL_HAS_GTSAM OFF)
endif()
```

**In `adapters/CMakeLists.txt`** (conditional):
```cmake
if(SPATIAL_HAS_GTSAM)
    add_subdirectory(gtsam)
endif()
```

### 1.4 Architecturally Appropriate Mechanism

Conan 2 with CMakeDeps is the established pattern (ADR-003). GTSAM should follow the same mechanism. GTSAM's Conan recipe is available in conan-center-index. No vendoring or FetchContent is appropriate.

---

## 2. Adapter Boundary

```
┌─────────────────────────────────────────────────────┐
│  Canonical P3 Domain (core/)                         │
│    trajectory.h, pose_graph.h, loop_closure.h,       │
│    optimization.h, trajectory_adapter.h              │
│    → NO GTSAM HEADERS, NO GTSAM TYPES               │
└───────────────────────┬─────────────────────────────┘
                        │ reads P3 types
                        ▼
┌─────────────────────────────────────────────────────┐
│  GTSAM Adapter (adapters/gtsam/)                     │
│    gtsam_optimizer_adapter.h/.cpp                    │
│    → includes: GTSAM headers + core/trajectory/*    │
│    → converts: P3 ↔ GTSAM at boundary               │
│    → ONLY location of GTSAM includes in the project │
└───────────────────────┬─────────────────────────────┘
                        │ GTSAM API calls
                        ▼
┌─────────────────────────────────────────────────────┐
│  GTSAM Library (third-party via Conan)               │
│    NonlinearFactorGraph, Values, Pose3, Rot3,        │
│    PriorFactor, BetweenFactor, LevenbergMarquardt,   │
│    Marginals, noiseModel                             │
│    → internal representation only                    │
└─────────────────────────────────────────────────────┘
```

**GTSAM types MUST NOT enter:**
- `core/` (any subdirectory)
- `core/trajectory/`
- `core/geometry/`
- `core/scene/`
- Canonical JSON schemas
- Any header included by `core/`

**Verified:** `core/trajectory/*.h` includes only `<array>`, `<cstdint>`, `<string>`, `<vector>`, and `core/reconstruction/reconstruction.h`. None reference GTSAM.

---

## 3. Canonical → GTSAM Type Mapping

### 3.1 Mapping Table

| P3 Type | P3 Fields | GTSAM Type | Conversion | Direction | Ownership | Lossless | Test |
|---|---|---|---|---|---|---|---|
| `TrajectoryPoseNode.position_xyz[3]` | `double[3]` | `gtsam::Point3` | `Point3(x,y,z)` | P3→GTSAM | adapter stack-local | Yes | Round-trip |
| `TrajectoryPoseNode.rotation_xyzw[4]` | `double[4]` xyzw | `gtsam::Rot3` | `Rot3(w,x,y,z)` | P3→GTSAM | adapter stack-local | Yes | Round-trip |
| Combined pose | position + rotation | `gtsam::Pose3` | `Pose3(Rot3(w,x,y,z), Point3(x,y,z))` | P3→GTSAM | adapter stack-local | Yes | Composition |
| `PoseGraphNode.node_id` | `int64_t` | `gtsam::Key` | `Key = static_cast<Key>(node_id)` | P3→GTSAM | adapter-internal | Yes (non-negative) | Identity |
| `PoseGraphEdge.relative_position_xyz[3]` | `double[3]` | `gtsam::Point3` (within `Pose3`) | Same as pose | P3→GTSAM | adapter stack-local | Yes | Relative |
| `PoseGraphEdge.relative_rotation_xyzw[4]` | `double[4]` | `gtsam::Rot3` (within `Pose3`) | Same as pose | P3→GTSAM | adapter stack-local | Yes | Relative |
| `PoseGraphEdge.type = "prior"` | edge | `gtsam::PriorFactor<Pose3>` | Build from edge + trajectory node | P3→GTSAM | graph-owned | Yes | Prior |
| `PoseGraphEdge.type ∈ {"odometry","loop_closure","lidar_odometry"}` | edge | `gtsam::BetweenFactor<Pose3>` | Build from relative Pose3 + noise | P3→GTSAM | graph-owned | Yes | Between |
| `PoseGraphEdge.type = "gps"` | edge | `gtsam::PriorFactor<Point3>` | Position-only from relative_position | P3→GTSAM | graph-owned | Yes (rotation unconstrained) | GPS |
| `PoseGraphEdge.type = "imu_preintegration"` | edge | **SKIPPED** | Not representable | N/A | N/A | N/A | Skip |
| `information_matrix_6x6[36]` | `double[36]` row-major [v,ω] | `gtsam::noiseModel::Gaussian::Information` | Permute P₆·Ω·P₆, then wrap | P3→GTSAM | factor-owned | Yes | Permutation |
| `gtsam::Values` (optimized) | Key → Pose3 | `OptimizedPoseNode` | Extract R,t → permute quaternion | GTSAM→P3 | adapter stack-local | Yes | Result |
| `gtsam::Marginals` | Key → 6×6 covariance | `covariance_position[6]`, `covariance_rotation[6]` | Permute P₆·Σ·P₆, extract upper triangle | GTSAM→P3 | adapter stack-local | Yes | Covariance |

### 3.2 What Is NOT Mapped

| P3 Entity | Reason | Future |
|---|---|---|
| `Trajectory` (metadata entity) | Not a GTSAM concept. Adapter reads trajectory metadata for provenance. | N/A |
| `PoseGraph` (metadata entity) | Not a GTSAM concept. Adapter reads graph metadata for provenance. | N/A |
| `LoopClosure` (domain entity) | Becomes a `PoseGraphEdge(type="loop_closure")` before entering the adapter. | P3-impl-5 |
| `OptimizationResult` (metadata entity) | Produced by the adapter after optimization, not consumed. | Output only |
| `TrajectoryUncertainty` | Aggregate summary, not per-pose. Computed from optimized covariances. | P3-impl-10 |

---

## 4. Pose Convention

### 4.1 P3 Convention (ADR-007)

```
T_A_B: "transform from frame B into frame A"
p_A = R · p_B + t
```

Trajectory poses: `T_trajectory_camera`:
```
p_trajectory = R_tc · p_camera + t_tc
```

This is **world-from-body** where `trajectory = world`, `camera = body`.

### 4.2 GTSAM Convention

```
Pose3(R, t): p_world = R · p_body + t
```

GTSAM `Pose3` is **world-from-body**.

### 4.3 Mathematical Proof of Direct Mapping

**Given:** P3 stores `T_trajectory_camera = (R_tc, t_tc)` where:
```
p_trajectory = R_tc · p_camera + t_tc      ... (P3)
```

**GTSAM:** `Pose3(R, t)` where:
```
p_world = R · p_body + t                    ... (GTSAM)
```

**Mapping:** Set `world ≡ trajectory`, `body ≡ camera`:
```
Pose3(R_tc, t_tc)
```

**Verification:** Transform camera-origin point `(0, 0, 0)`:
- P3: `p_trajectory = R_tc · 0 + t_tc = t_tc`
- GTSAM: `Pose3(R_tc, t_tc).transformFrom(Point3(0,0,0)) = t_tc`

**Result is identical. No inversion, no reordering of R and t.** ∎

### 4.4 Relative Transforms (Edges)

P3 edge: `relative_position_xyz` + `relative_rotation_xyzw` encode `T_source_target`.

GTSAM `BetweenFactor<Pose3>(X_i, X_j, T_ij, noise)`:
```
residual: T_ij · X_j ≈ X_i
```

P3 `T_source_target`: "a point in target frame, expressed in source frame":
```
p_source = R_st · p_target + t_st
```

These are the **same convention.** The relative `Pose3` is constructed directly from `relative_position_xyz` and `relative_rotation_xyzw` with quaternion reorder only. ∎

### 4.5 Reconciliation with SE3/RigidTransform

`core/geometry/se3.h` defines `SE3(Rotation, Translation)` with `p' = R * p + t` (same convention). `SE3::ToMatrix()` produces the 4×4 homogeneous matrix. The GTSAM adapter can construct `Pose3` directly from the (R, t) pair without going through the matrix form.

---

## 5. Quaternion Convention

### 5.1 Verified GTSAM API

GTSAM `Rot3` constructor (verified against GTSAM 4.x API):
```cpp
// Scalar-first: w first
static Rot3 Quaternion(double w, double x, double y, double z);
```

GTSAM `Rot3` extraction:
```cpp
// Returns [w, x, y, z]
static Vector4 ToQuaternion(const Rot3& R);
```

### 5.2 Conversion: P3 → GTSAM

```
P3: (x, y, z, w) = (q[0], q[1], q[2], q[3])
GTSAM: Rot3::Quaternion(w, x, y, z) = Rot3::Quaternion(q[3], q[0], q[1], q[2])
```

### 5.3 Conversion: GTSAM → P3

```
GTSAM: ToQuaternion(R) = [w, x, y, z]
P3: (x, y, z, w) = (result[1], result[2], result[3], result[0])
```

### 5.4 Round-Trip Proof

```
P3: (x,y,z,w) → Rot3(w,x,y,z) → ToQuaternion → (w,x,y,z) → P3: (x,y,z,w)
Index mapping: [0,1,2,3] → args [3,0,1,2] → result [1,2,3,0] → [0,1,2,3] ✓
```

### 5.5 Non-Trivial Test Case

90° rotation about Z-axis:
```
P3: (0, 0, sin(π/4), cos(π/4)) = (0, 0, 0.7071, 0.7071)
GTSAM: Rot3::Quaternion(0.7071, 0, 0, 0.7071)
```

Transform `(1, 0, 0)`:
- P3: R · (1,0,0) = (0, 1, 0) (camera +x → trajectory +y)
- GTSAM: Rot3 rotation of Point3(1,0,0) = Point3(0,1,0) ✓

---

## 6. Tangent-Space Ordering

### 6.1 Definitions

**P3 tangent vector** (ADR-007, used in `information_matrix_6x6` and `covariance_position/rotation`):
```
ξ_P3 = [v_x, v_y, v_z, ω_x, ω_y, ω_z]    (6×1)
```

**GTSAM tangent vector** (`Pose3::Logmap` ordering):
```
ξ_GTSAM = [ω_x, ω_y, ω_z, v_x, v_y, v_z]    (6×1)
```

### 6.2 Permutation Matrix P₆

```
         P3 index:  0  1  2  3  4  5
                    v  v  v  ω  ω  ω

        ┌                              ┐
        │  0  0  0  1  0  0 │   GTSAM[0] = P3[3]  (ω_x)
        │  0  0  0  0  1  0 │   GTSAM[1] = P3[4]  (ω_y)
P₆  =   │  0  0  0  0  0  1 │   GTSAM[2] = P3[5]  (ω_z)
        │  1  0  0  0  0  0 │   GTSAM[3] = P3[0]  (v_x)
        │  0  1  0  0  0  0 │   GTSAM[4] = P3[1]  (v_y)
        │  0  0  1  0  0  0 │   GTSAM[5] = P3[2]  (v_z)
        └                              ┘

GTSAM index:  0  1  2  3  4  5
              ω  ω  ω  v  v  v
```

**Properties:** `P₆ = P₆ᵀ = P₆⁻¹` (self-inverse).

### 6.3 Tangent Vector Conversion

```
ξ_GTSAM = P₆ · ξ_P3
ξ_P3    = P₆ · ξ_GTSAM    (since P₆ = P₆⁻¹)
```

### 6.4 Effect on Covariance

Under basis change `ξ_new = P · ξ_old`:
```
Σ_new = P · Σ_old · Pᵀ
```

Therefore:
```
Σ_GTSAM = P₆ · Σ_P3 · P₆
Σ_P3    = P₆ · Σ_GTSAM · P₆
```

### 6.5 Effect on Information Matrix

Since `Ω = Σ⁻¹`:
```
Ω_GTSAM = P₆ · Ω_P3 · P₆
Ω_P3    = P₆ · Ω_GTSAM · P₆
```

**Derivation:**
```
χ² = ξ_P3ᵀ · Ω_P3 · ξ_P3
   = (P₆ · ξ_GTSAM)ᵀ · Ω_P3 · (P₆ · ξ_GTSAM)
   = ξ_GTSAMᵀ · P₆ · Ω_P3 · P₆ · ξ_GTSAM
   = ξ_GTSAMᵀ · Ω_GTSAM · ξ_GTSAM

∴ Ω_GTSAM = P₆ · Ω_P3 · P₆
```

### 6.6 Effect on Jacobians

GTSAM computes Jacobians in its own tangent space `[ω, v]`. The permutation affects:
- How the adapter **interprets** the information matrix passed to noise model construction
- How the adapter **extracts** marginal covariances from GTSAM results

The adapter does NOT modify GTSAM's internal Jacobian computation. GTSAM works entirely in `[ω, v]` internally. The permutation is applied only at the **boundary** when converting P3 ↔ GTSAM representations.

### 6.7 Code

```cpp
constexpr int kTangentPermutation[6] = {3, 4, 5, 0, 1, 2};

void permuteMatrixP3ToGtsam(const double in[36], double out[36]) {
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 6; ++c)
            out[r * 6 + c] = in[kTangentPermutation[r] * 6 + kTangentPermutation[c]];
}

// Inverse is identical (P₆ = P₆⁻¹)
void permuteMatrixGtsamToP3(const double in[36], double out[36]) {
    permuteMatrixP3ToGtsam(in, out);
}
```

---

## 7. Information / Covariance Conversion

### 7.1 Complete Conversion Path

```
P3 information_matrix_6x6[36]
    (row-major, [v,ω] ordering)
        │
        ▼  permuteMatrixP3ToGtsam()
GTSAM-ordered information matrix[36]
    (row-major, [ω,v] ordering)
        │
        ▼  Eigen::Map<Matrix6d>
Eigen::Matrix6d info_gtsam
        │
        ▼  noiseModel::Gaussian::Information(info_gtsam)
gtsam::noiseModel::Base::shared_ptr noiseModel
        │
        ▼  passed to PriorFactor / BetweenFactor
    [GTSAM optimization runs in [ω,v] space]
        │
        ▼  Marginals::marginalCovariance(key)
Eigen::Matrix6d cov_gtsam
    (in [ω,v] ordering)
        │
        ▼  permuteMatrixGtsamToP3()
Eigen::Matrix6d cov_p3
    (in [v,ω] ordering)
        │
        ▼  extract upper triangle
P3 covariance_position[6] = [xx,xy,xz,yy,yz,zz]
P3 covariance_rotation[6] = [xx,xy,xz,yy,yz,zz]
```

### 7.2 Matrix Type at Each Stage

| Stage | Matrix type | Ordering | Notes |
|---|---|---|---|
| P3 input | Information | [v,ω] | From `PoseGraphEdge.information_matrix_6x6` |
| After permutation | Information | [ω,v] | Ready for GTSAM |
| GTSAM noise model | SqrtInformation (internal) | [ω,v] | GTSAM internally computes Cholesky |
| GTSAM marginals output | Covariance | [ω,v] | From `Marginals::marginalCovariance()` |
| After inverse permutation | Covariance | [v,ω] | Ready for P3 |
| P3 output | Covariance (upper triangle) | [v,ω] | Stored in `OptimizedPoseNode` |

### 7.3 Preventing Double Inversion

**Risk:** Confusing information and covariance matrices.

**Mitigation:**
- P3 edges store **information** (D-PG-05). The adapter converts information → noise model directly.
- GTSAM marginals return **covariance**. The adapter converts covariance → P3 covariance directly.
- **There is NO inversion step in the adapter.** Information stays information, covariance stays covariance. The only operation is the tangent-space permutation.

**GTSAM's `noiseModel::Gaussian::Information(Eigen::Matrix)`** accepts the information matrix directly and handles internal factorization. The adapter must NOT pass the Cholesky factor or square root.

---

## 8. Factor Mapping

### 8.1 PriorFactor

| Component | Value | Source |
|---|---|---|
| Factor type | `gtsam::PriorFactor<Pose3>` | Dispatched on `edge.type == "prior"` |
| Key | `static_cast<Key>(edge.source_node_id)` | The node being anchored |
| Prior value | `Pose3(R, t)` from trajectory node matching `edge.source_node_id` | Trajectory payload |
| Noise model | From `edge.information_matrix_6x6` permuted to GTSAM ordering | Edge payload |
| `target_node_id` | Must be 0 or ignored (prior is unary) | Verified, logged if non-zero |

### 8.2 BetweenFactor

| Component | Value | Source |
|---|---|---|
| Factor type | `gtsam::BetweenFactor<Pose3>` | Dispatched on `edge.type ∈ {"odometry","loop_closure","lidar_odometry"}` |
| Source key | `static_cast<Key>(edge.source_node_id)` | Edge payload |
| Target key | `static_cast<Key>(edge.target_node_id)` | Edge payload |
| Measurement | `Pose3(R, t)` from `edge.relative_position_xyz` + `edge.relative_rotation_xyzw` | Edge payload (quaternion reordered) |
| Noise model | From `edge.information_matrix_6x6` permuted to GTSAM ordering | Edge payload |

### 8.3 Loop Closure

Loop closures are **identical to BetweenFactor** in GTSAM. The distinction is:

| Aspect | Odometry edge | Loop closure edge |
|---|---|---|
| GTSAM factor | `BetweenFactor<Pose3>` | `BetweenFactor<Pose3>` |
| Noise model | Typically high information (tight) | Typically lower information (looser) |
| Node connectivity | Sequential (i → i+1) | Non-sequential (i → j, |i-j| >> 1) |
| Provenance | `source = "visual_odometry"` | `source = "loop_closure_detector"` |
| P3 identity | `edge.type = "odometry"` | `edge.type = "loop_closure"` |

The adapter dispatches on `edge.type` for logging and provenance, but the mathematical construction is identical.

### 8.4 GPS Factor

| Component | Value | Source |
|---|---|---|
| Factor type | `gtsam::PriorFactor<Point3>` | Dispatched on `edge.type == "gps"` |
| Key | `static_cast<Key>(edge.source_node_id)` | The node being constrained |
| Point value | `Point3(relative_position_xyz[0], [1], [2])` | Edge payload |
| Noise model | 3×3 from top-left block of permuted information matrix | Edge payload |

**Note:** GPS only constrains translation. Rotation is unconstrained. The adapter extracts the 3×3 position block from the 6×6 permuted information matrix.

### 8.5 Factor IDs

GTSAM does not assign factor IDs. Factors are identified by their position in the `NonlinearFactorGraph` (index). For provenance, the adapter maintains an internal mapping: `factor_index → edge_id`.

### 8.6 Robust Kernels

**P3 does not currently model robust kernels** (ADR-005 research gate, F-28). GTSAM supports `noiseModel::Robust(kernel, baseModel)`. The adapter MAY optionally wrap noise models with robust kernels if configured, but this is not part of the P3-impl-6 scope. If implemented, the kernel configuration goes into `OptimizationProvenance.backend_specific_json`.

---

## 9. PoseGraph ↔ Trajectory Join

### 9.1 Problem

`PoseGraphNode` has `node_id`, `frame_id`, `timestamp_ns`.  
`TrajectoryPoseNode` has `frame_id`, `timestamp_ns`, `sequence_index`.  

The adapter must find the initial `T_trajectory_camera` for each graph node to initialize GTSAM `Values`.

### 9.2 Primary Strategy: Join by `frame_id`

```cpp
// Build lookup from trajectory payload
std::unordered_map<std::string, size_t> frame_to_traj_index;
for (size_t i = 0; i < trajectory_nodes.size(); ++i) {
    frame_to_traj_index[trajectory_nodes[i].frame_id] = i;
}

// For each graph node, find initial pose
for (const auto& graph_node : graph_nodes) {
    auto it = frame_to_traj_index.find(graph_node.frame_id);
    if (it != frame_to_traj_index.end()) {
        const auto& traj_node = trajectory_nodes[it->second];
        values.insert<Key>(graph_node.node_id,
            Pose3FromTrajectoryNode(traj_node));
    }
}
```

### 9.3 Fallback: Index Alignment

When `frame_id` is empty (some backends may not populate it):
```
graph_node.node_id == trajectory_node.sequence_index
```

**Recorded in provenance:** `"join_strategy": "index_alignment"`.

### 9.4 Why Not Always Index Alignment

P3 design (D-PG-03) guarantees nodes are ordered by `sequence_index` but does NOT guarantee `node_id == sequence_index`. A backend could assign `node_id = 17, 18, 19` while `sequence_index = 0, 1, 2`. The `frame_id` join is stable regardless.

---

## 10. Optimization Result → Canonical

### 10.1 Extraction

| GTSAM output | P3 field | Conversion | Test |
|---|---|---|---|
| `result.at<Pose3>(key)` | `position_xyz` | `Pose3.translation()` → `array<double,3>` | Yes |
| `result.at<Pose3>(key)` | `rotation_xyzw` | `Rot3::ToQuaternion()` → reorder → `array<double,4>` | Yes |
| `result.at<Pose3>(key)` | `frame_id` | Look up `PoseGraphNode` by key → `frame_id` | Yes |
| Same | `timestamp_ns` | Look up `PoseGraphNode` by key → `timestamp_ns` | Yes |
| Same | `sequence_index` | Look up `PoseGraphNode` by key → `sequence_index` | Yes |
| `Marginals::marginalCovariance(key)` | `covariance_position[6]` | Permute 6×6, extract 3×3 upper triangle (position block) | Yes (when computed) |
| Same | `covariance_rotation[6]` | Permute 6×6, extract 3×3 upper triangle (rotation block) | Yes |
| `graph.error(initial_values)` | `initial_error` | Direct double | Yes |
| `graph.error(result)` | `final_error` | Direct double | Yes |
| `(initial - final) / initial` | `error_reduction` | Computed | Yes |
| `optimizer.iterations()` | `iterations` | Direct int64 | Yes |

### 10.2 Identity Preservation

Each `OptimizedPoseNode` preserves:
- `frame_id` — from the original `PoseGraphNode` (UUID bridge, D-TRJ-10)
- `timestamp_ns` — from the original `PoseGraphNode`
- `sequence_index` — from the original `PoseGraphNode`

The GTSAM `Key` (integer) is **adapter-internal only** and never appears in the canonical output.

---

## 11. Artifact / CAS Model

### 11.1 Optimized Trajectory Artifact

**Type:** `OptimizedTrajectoryPayload` (CAS document)

```json
{
  "schema_version": 1,
  "trajectory_id": "<uuid of original trajectory>",
  "optimization_result_id": "<uuid of OptimizationResult>",
  "nodes": [
    {
      "frame_id": "<uuid>",
      "timestamp_ns": 0,
      "sequence_index": 0,
      "position_xyz": [1.0, 2.0, 3.0],
      "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
      "covariance_position": [0.01, 0.0, 0.0, 0.01, 0.0, 0.01],
      "covariance_rotation": [0.001, 0.0, 0.0, 0.001, 0.0, 0.001]
    }
  ]
}
```

### 11.2 Optimization Result Artifact

**Type:** `OptimizationResult` (metadata row + CAS document)

**Required:** Yes — `optimization_result.schema.json` must be created during P3-impl-6 implementation.

**Do NOT create the schema in this mapping phase.**

### 11.3 CAS Boundary

```
P3 canonical Trajectory (existing CAS)
    ↓ adapter reads
GTSAM adapter
    ↓ optimizes
GTSAM Values (optimized)
    ↓ adapter extracts
OptimizedPoseNode[] (adapter stack-local)
    ↓ serialized as
OptimizedTrajectoryPayload (NEW CAS document)
    ↓ referenced by
OptimizationResult (NEW metadata row)
```

GTSAM-specific data (optimizer config, internal state) → `OptimizationProvenance.backend_specific_json`.  
Canonical data (poses, covariances) → CAS payload.

---

## 12. Provenance and Determinism

### 12.1 Provenance Fields

```json
{
  "backend": {
    "name": "gtsam",
    "version": "<GTSAM version from cmake>"
  },
  "adapter_version": "0.1.0",
  "configuration_hash": "<SHA-256>",
  "input_artifact_hashes": [
    "<trajectory_cas_hash>",
    "<pose_graph_cas_hash>"
  ],
  "engine_version": "<from build>",
  "engine_commit": "<from build>",
  "git_commit": "<adapter git commit>",
  "started_at_ns": "<epoch ns>",
  "finished_at_ns": "<epoch ns>",
  "duration_ns": "<delta ns>",
  "backend_specific_json": "{\"optimizer\": \"LevenbergMarquardt\", ...}"
}
```

### 12.2 Deterministic Configuration Hash

```
SHA-256(
    optimizer_name +
    max_iterations +
    relative_error_tol +
    absolute_error_tol +
    ordering +
    SHA-256(all edges: type + src + tgt + rel_pos + rel_rot + info_matrix) +
    SHA-256(all poses: position + rotation)
)
```

**Same inputs + same config → same hash.**

### 12.3 Deterministic Behavior

- **Node insertion order:** By `node_id` (ascending). Deterministic.
- **Edge iteration order:** By `edge_id` (ascending). Deterministic.
- **Optimizer params:** Fixed `LevenbergMarquardtParams` with `Ordering::COLAMD`. Deterministic.
- **No threading randomness:** LM is single-threaded by default in GTSAM.
- **Anchor:** Always `node_id = 0` with tight prior (information = `1e6 · I₆`).

---

## 13. Dependency Architecture

### 13.1 Proposed Structure

```
adapters/gtsam/
    CMakeLists.txt
    gtsam_optimizer_adapter.h
    gtsam_optimizer_adapter.cpp
    gtsam_adapter_build_info.h.in
```

### 13.2 Library Target

```cmake
# adapters/gtsam/CMakeLists.txt
find_package(GTSAM CONFIG REQUIRED)

configure_file(gtsam_adapter_build_info.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/gtsam_adapter_build_info.h" @ONLY)

add_library(spatial_gtsam_adapter STATIC
    gtsam_optimizer_adapter.cpp
)
target_include_directories(spatial_gtsam_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(spatial_gtsam_adapter PUBLIC
    spatial_core
    GTSAM::GTSAM
)
set_target_properties(spatial_gtsam_adapter PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
```

### 13.3 Include Visibility

- `gtsam_optimizer_adapter.h` includes `core/trajectory/*.h` (P3 types) + GTSAM headers
- `spatial_core` public headers do NOT include GTSAM
- Consumer code (tests, workers) includes only `gtsam_optimizer_adapter.h`

### 13.4 Platform Concerns

- GTSAM supports Windows (MSVC), Linux, macOS. Conan recipe handles platform detection.
- GTSAM requires Eigen (already present as Conan dependency).
- GTSAM optionally uses Boost (Conan recipe pulls it if needed). Boost is NOT currently in conanfile.txt — this is a new transitive dependency to track.

---

## 14. Test Architecture

### 14.1 Separate GTSAM Test Target

**`spatial_unit_tests` MUST NOT link GTSAM.** It must remain runnable without GTSAM installed.

```cmake
# In adapters/gtsam/CMakeLists.txt or tests/CMakeLists.txt:
add_executable(spatial_gtsam_tests
    unit/test_gtsam_optimizer_adapter.cpp
)
target_link_libraries(spatial_gtsam_tests PRIVATE
    spatial_gtsam_adapter
    spatial_core
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(spatial_gtsam_tests PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(spatial_gtsam_tests)
```

### 14.2 Test Matrix

| # | Test | What it verifies |
|---|---|---|
| 1 | `QuaternionRoundTrip` | P3 (x,y,z,w) → GTSAM Rot3 → P3 (x,y,z,w) |
| 2 | `QuaternionNonTrivial` | 90°Z rotation transforms (1,0,0)→(0,1,0) |
| 3 | `Quaternion180Z` | 180°Z rotation correctness |
| 4 | `Quaternion90X` | 90°X rotation correctness |
| 5 | `Pose3Direct` | T_trajectory_camera maps to Pose3 without inversion |
| 6 | `Pose3TranslationOnly` | Pure translation round-trip |
| 7 | `TangentPermutationDiagonal` | P6·diag(100,100,100,10,10,10)·P6 = diag(10,10,10,100,100,100) |
| 8 | `TangentPermutationRoundTrip` | P6·M·P6 = M for arbitrary SPD matrix |
| 9 | `TangentPermutationNonDiagonal` | Off-diagonal coupling preserved correctly |
| 10 | `InformationMatrixConversion` | P3 info → noise model → GTSAM info matches |
| 11 | `CovariancePermutation` | P3 cov → P6·Σ·P6 → GTSAM cov correct |
| 12 | `CovarianceUpperTriangle` | 3×3 upper triangle extraction after 6×6 permutation |
| 13 | `PriorFactorMapping` | Single node with prior stays at prior value |
| 14 | `BetweenFactorMapping` | Two nodes with between edge: relative pose preserved |
| 15 | `LoopClosureFactorMapping` | Same as between, different type string |
| 16 | `GpsFactorMapping` | Position-only constraint correct |
| 17 | `ImuEdgeSkipped` | IMU edge logged and skipped, optimization proceeds |
| 18 | `SmallGraphOptimization` | 3-node chain with odometry, verify positions |
| 19 | `LoopClosureCorrectsDrift` | 3 nodes + drift + loop closure → error reduction |
| 20 | `ResultExtraction` | Optimized Values → TrajectoryPoseNode with correct frame_id, timestamp |
| 21 | `FrameIdentityPreserved` | Optimized nodes have correct frame_id UUIDs |
| 22 | `NodeOrderingPreserved` | Optimized nodes have correct sequence_index |
| 23 | `EmptyGraph` | 0 nodes → status="failed" |
| 24 | `NoEdges` | Nodes but 0 edges → status="failed" |
| 25 | `DisconnectedGraph` | Two disconnected components → status="converged" with warning |
| 26 | `InvalidNodeReference` | Edge references non-existent node → edge skipped |
| 27 | `DeterministicHash` | Same inputs → same configuration_hash |
| 28 | `ProvenanceFields` | All provenance fields populated |
| 29 | `SchemaValidation` | OptimizationResult validates against schema |
| 30 | `ArchitectureBoundary` | `core/trajectory/` has zero `#include.*gtsam` |

---

## 15. Existing Architecture Compatibility

### 15.1 Verified Compatible

| Component | Compatibility | Evidence |
|---|---|---|
| P2.5 Reconstruction | ✅ No change | GTSAM adapter produces `OptimizedPoseNode`, not `Reconstruction` |
| P3 Trajectory | ✅ Reads | Adapter consumes trajectory for initial poses |
| P3 PoseGraph | ✅ Reads | Adapter consumes pose graph for constraints |
| P3 LoopClosure | ✅ Via PoseGraphEdge | Loop closures become edges before adapter |
| ADR-005 | ✅ Follows | GTSAM is the designated optimizer |
| ADR-007 | ✅ Preserved | Quaternion and tangent conventions enforced at adapter boundary |
| ADR-018 | ✅ Preserved | No raw Eigen in domain code; Eigen only inside adapter |
| ADR-024 | ✅ Compatible | Observation graph is separate substrate; optimizer reads pose graph |
| COLMAP trajectory adapter | ✅ Unchanged | P3-impl-3 is untouched |

### 15.2 What MUST NOT Change

- `core/trajectory/*.h` — no GTSAM includes
- `core/geometry/*.h` — no GTSAM includes
- `adapters/colmap/` — no GTSAM references
- `schemas/json/trajectory.schema.json` — unchanged
- `schemas/json/pose-graph.schema.json` — unchanged
- `schemas/json/loop-closure.schema.json` — unchanged
- All existing tests (536/536 baseline) — must remain green

---

## 16. Open Questions / Required Decisions

### A. Can Be Resolved Inside Implementation

| # | Question | Resolution |
|---|---|---|
| A1 | Exact GTSAM Conan version | Use latest stable 4.2.x; pin in conan.lock |
| A2 | Boost transitive dependency | Let Conan resolve; verify no conflicts with existing deps |
| A3 | Exact optimizer params (max iterations, tolerances) | Defaults: 100 iterations, 1e-10 relative/absolute tol. Configurable via `OptimizationInput.config_json` |
| A4 | Factor index → edge_id mapping | Internal `std::vector` in adapter; trivial |

### B. Requires Architectural Decision

| # | Question | Impact | Proposed |
|---|---|---|---|
| B1 | Should `optimization_result.schema.json` be created in P3-impl-6 or deferred? | Required for CAS persistence of optimization results | **Create in P3-impl-6.** Without it, optimized trajectory cannot be persisted. |
| B2 | Should the adapter support robust kernels in P3-impl-6? | Noise model construction | **Defer.** No robust kernels in P3-impl-6. Record in provenance if added later. |
| B3 | What is the default anchor prior information value? | Optimization behavior | **`1e6 · I₆`** (very tight). Document as convention. |

### C. Should Be Deferred

| # | Question | When |
|---|---|---|
| C1 | IMU preintegration canonical type | P3-impl-9+ (with KISS-ICP or dedicated IMU adapter) |
| C2 | iSAM2 incremental optimization | P3-impl-6+ (streaming increment) |
| C3 | Marginals for graphs > 1000 nodes | P3-impl-10 (uncertainty propagation) |
| C4 | Multi-session graph semantics | P3-impl-8 |
| C5 | Robust kernel configuration/provenance | P3-impl-6+ (when robust kernels are added) |

---

## 17. Implementation Plan

### P3-impl-6a: Dependency + Build Isolation

**Scope:**
- Add GTSAM to `conanfile.txt`
- Add `find_package(GTSAM CONFIG)` to root `CMakeLists.txt` (conditional)
- Create `adapters/gtsam/CMakeLists.txt`
- Add conditional `add_subdirectory(gtsam)` to `adapters/CMakeLists.txt`
- Verify `spatial_unit_tests` still builds without GTSAM linkage
- Verify `core/` has no GTSAM dependency

**Exit criteria:** `cmake --build` succeeds; `spatial_unit_tests` still passes; `spatial_gtsam_tests` compiles (empty or minimal).

### P3-impl-6b: GTSAM Adapter Core + Converters

**Scope:**
- `gtsam_optimizer_adapter.h` — public interface (function signatures, result types)
- `gtsam_optimizer_adapter.cpp` — implementation skeleton
- Quaternion conversion (P3 ↔ GTSAM)
- Pose3 conversion (P3 T_trajectory_camera ↔ GTSAM Pose3)
- Tangent-space permutation P₆
- Information matrix permutation
- Covariance matrix permutation

**Exit criteria:** All conversion functions implemented and unit-tested (tests 1–12).

### P3-impl-6c: Factor Graph Construction

**Scope:**
- Read `PoseGraph` + `Trajectory` → build `NonlinearFactorGraph` + `Values`
- Join strategy (frame_id primary, index fallback)
- Dispatch: PriorFactor, BetweenFactor, GPS Factor
- Skip IMU edges with logging
- Anchor node 0 with tight prior
- Deterministic node/edge ordering

**Exit criteria:** Graph construction tests pass (tests 13–17).

### P3-impl-6d: Optimization

**Scope:**
- Configure `LevenbergMarquardtParams` (deterministic)
- Run `LevenbergMarquardtOptimizer::optimize()`
- Extract `initial_error`, `final_error`, `iterations`
- Map status: converged/failed/diverged
- Handle exceptions gracefully

**Exit criteria:** Optimization tests pass (tests 18–19).

### P3-impl-6e: Canonical Result Extraction

**Scope:**
- Extract optimized `Pose3` → `OptimizedPoseNode`
- Extract marginals (when graph small enough)
- Preserve frame_id, timestamp_ns, sequence_index
- Build `OptimizationResult` metadata
- Compute configuration hash

**Exit criteria:** Result extraction tests pass (tests 20–23).

### P3-impl-6f: Artifact / Provenance Integration

**Scope:**
- Create `schemas/json/optimization_result.schema.json`
- Serialize `OptimizedTrajectoryPayload` (CAS document format)
- Populate `OptimizationProvenance` (all fields)
- Validate against schema

**Exit criteria:** Schema validation tests pass (tests 28–29).

### P3-impl-6g: Full Test Suite + Architecture Gates

**Scope:**
- Complete all 30 tests
- Run `spatial_unit_tests` (must remain 536+ PASS)
- Run `spatial_gtsam_tests` (new, must PASS)
- Architecture boundary test (test 30)
- Build Debug + Release
- Write Verification Report

**Exit criteria:** All tests pass; verification report produced.

---

## 18. Scope Guardrails

**This mapping does NOT authorize:**
- Code changes (deferred to implementation phase)
- CMake changes (deferred to P3-impl-6a)
- Dependency changes (deferred to P3-impl-6a)
- Schema creation (deferred to P3-impl-6f)
- DB migrations (not needed for P3-impl-6)
- Changes to P2.5 types or schemas
- Changes to P3 canonical types (`core/trajectory/*.h`)
- Changes to COLMAP adapter (`adapters/colmap/`)
- ORB-SLAM3 adapter (P3-impl-4)
- KISS-ICP adapter (P3-impl-9)
- Loop closure adapter (P3-impl-5)
- Multi-session implementation (P3-impl-8)
- Uncertainty propagation (P3-impl-10)
- IMU preintegration (future increment)
- iSAM2 incremental optimization (future increment)

---

## 19. Final Verdict

### **READY FOR IMPLEMENTATION**

**Rationale:**

1. All 19 mapping sections are complete with concrete, repository-specific details.
2. SE(3) direction is mathematically proven (§4.3, §4.4).
3. Quaternion conversion is specified with API-level detail (§5.2, §5.3).
4. Tangent-space permutation is derived with the 6×6 matrix P₆ and code (§6.2, §6.7).
5. Information/covariance conversion path is fully specified with no double-inversion risk (§7).
6. GTSAM dependency is isolated to `adapters/gtsam/` only (§2, §13).
7. `spatial_unit_tests` remains GTSAM-independent (§14.1).
8. All open questions are categorized (§16): B1 (schema) and B3 (anchor value) can be resolved during implementation.
9. Implementation plan has 7 safe substeps with exit criteria (§17).
10. No architecture blockers identified.

**Architectural decisions required before implementation:**
- **B1:** Create `optimization_result.schema.json` in P3-impl-6 (proposed: yes)
- **B3:** Default anchor prior information value (proposed: `1e6 · I₆`)

**Proposed next step:** Authorize P3-impl-6a (dependency + build isolation) after approving this mapping.

---

*This mapping was produced by opencode on 2026-08-19. No code, schema, dependency, or build changes were made.*
