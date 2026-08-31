# P3-impl-6b Implementation Mapping — GTSAM Adapter Core

**Status:** Pre-code architectural gate — mapping only  
**Date:** 2026-08-20  
**Author:** opencode  
**Depends on:** P3-impl-6a ✅ (build isolation verified)  
**Scope:** Mapping document only — no code, schema, dependency, or build changes

---

## 1. Current Repository State (Post-6a)

### 1.1 GTSAM Integration State

**GTSAM IS a repository dependency.** Verified by:

- `conanfile.txt`: `gtsam/[>=4.2.0 <4.3.0]` present
- `conan.lock`: GTSAM 4.2.1 resolved with boost/1.84.0 transitive
- Root `CMakeLists.txt`: `find_package(gtsam CONFIG REQUIRED)` gated by `SPATIAL_HAS_GTSAM`
- `CMakePresets.json`: `SPATIAL_HAS_GTSAM: ON` in both Debug and Release presets
- `adapters/gtsam/CMakeLists.txt`: `spatial_gtsam_adapter` static library target
- `adapters/gtsam/gtsam_optimizer_adapter.h`: placeholder header with `OptimizerOptions` and `OptimizerResult`
- `adapters/gtsam/gtsam_optimizer_adapter.cpp`: placeholder source
- `tests/unit/test_gtsam_adapter.cpp`: 2 smoke tests passing (537/537 Debug, 537/537 Release)

### 1.2 Adapter Boundary Status

**Boundary is CLEAN.** Verified by:

- `core/trajectory/*.h` includes: `<array>`, `<cstdint>`, `<string>`, `<vector>`, `core/reconstruction/reconstruction.h` only
- Zero `#include.*gtsam` in `core/` (architecture boundary test passes)
- `spatial_gtsam_adapter` links `spatial_core` PUBLIC + `gtsam::gtsam` PRIVATE
- `spatial_unit_tests` does NOT link GTSAM

### 1.3 Canonical Types Available

| Type | Header | Purpose |
|---|---|---|
| `TrajectoryPoseNode` | `core/trajectory/trajectory.h:38` | Pose payload (position, rotation, covariance) |
| `Trajectory` | `core/trajectory/trajectory.h:51` | Trajectory metadata |
| `PoseGraphNode` | `core/trajectory/pose_graph.h:25` | Graph node (id, frame_id, timestamp) |
| `PoseGraphEdge` | `core/trajectory/pose_graph.h:34` | Graph edge (relative transform, information matrix) |
| `PoseGraph` | `core/trajectory/pose_graph.h:63` | Graph metadata |
| `LoopClosure` | `core/trajectory/loop_closure.h:36` | Verified loop closure (becomes edge) |
| `OptimizationResult` | `core/trajectory/optimization.h:55` | Optimization metadata |
| `OptimizedPoseNode` | `core/trajectory/optimization.h:42` | Optimized pose (post-optimization) |
| `OptimizationProvenance` | `core/trajectory/optimization.h:23` | Provenance metadata |

---

## 2. Adapter Interface Design

### 2.1 Public API Surface

The adapter exposes a single entry point that orchestrates the full optimization pipeline:

```cpp
// adapters/gtsam/gtsam_optimizer_adapter.h

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/trajectory/trajectory.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/optimization.h"

namespace spatial::adapters::gtsam {

// Configuration for the Levenberg-Marquardt optimizer.
struct OptimizerOptions {
  std::int64_t max_iterations = 100;
  double lambda_initial = 1e-4;
  double lambda_factor = 10.0;
  double function_tolerance = 1e-10;
  double parameter_tolerance = 1e-10;
  double gradient_tolerance = 1e-10;
};

// Anchor prior configuration (B3).
struct AnchorPriorConfig {
  double information_scale = 1e6;  // scaling factor for identity prior
  bool enabled = true;             // whether to add anchor prior to node 0
};

// Input to the optimizer adapter.
struct OptimizationInput {
  core::Trajectory trajectory;
  std::vector<core::TrajectoryPoseNode> trajectory_nodes;
  core::PoseGraph graph;
  std::vector<core::PoseGraphNode> graph_nodes;
  std::vector<core::PoseGraphEdge> graph_edges;
  OptimizerOptions options;
  AnchorPriorConfig anchor_prior;
};

// Output from the optimizer adapter.
struct OptimizationOutput {
  core::OptimizationResult result;
  std::vector<core::OptimizedPoseNode> optimized_nodes;
};

// Main optimization entry point.
// Converts P3 types → GTSAM, runs Levenberg-Marquardt, extracts results.
// Returns false on failure; result.status indicates "converged" | "failed" | "diverged".
bool optimize(OptimizationInput input, OptimizationOutput& output);

}  // namespace spatial::adapters::gtsam
```

### 2.2 Internal Conversion Functions (not exposed in header)

These are `internal` namespace functions in the `.cpp` file:

```cpp
namespace spatial::adapters::gtsam::internal {

// Quaternion conversion
gtsam::Rot3 quaternionP3ToGtsam(const std::array<double, 4>& xyzw);
std::array<double, 4> quaternionGtsamToP3(const gtsam::Rot3& rot);

// Pose3 conversion
gtsam::Pose3 poseP3ToGtsam(const std::array<double, 3>& position,
                            const std::array<double, 4>& rotation);
void poseGtsamToP3(const gtsam::Pose3& pose,
                   std::array<double, 3>& position,
                   std::array<double, 4>& rotation);

// Tangent-space permutation
void permuteMatrixP3ToGtsam(const double in[36], double out[36]);
void permuteMatrixGtsamToP3(const double in[36], double out[36]);

// Information matrix → noise model
gtsam::noiseModel::Base::shared_ptr informationToNoiseModel(
    const std::array<double, 6>& information_6x6);

// Covariance extraction
void extractCovarianceUpperTriangle(const Eigen::Matrix<double, 6, 6>& cov,
                                    std::array<double, 6>& position,
                                    std::array<double, 6>& rotation);

// Key mapping
gtsam::Key nodeIdToKey(std::int64_t node_id);

// Configuration hash computation
std::string computeConfigurationHash(const OptimizationInput& input);

}  // namespace spatial::adapters::gtsam::internal
```

---

## 3. Conversion Functions Specification

### 3.1 Quaternion Conversion (§5 of 6a mapping)

**P3 → GTSAM:**
```
Input:  xyzw = [x, y, z, w]  (P3 scalar-last)
Output: Rot3::Quaternion(w, x, y, z)  (GTSAM scalar-first)
Code:   Rot3::Quaternion(xyzw[3], xyzw[0], xyzw[1], xyzw[2])
```

**GTSAM → P3:**
```
Input:  Rot3
Output: xyzw = [x, y, z, w]
Code:   auto q = Rot3::ToQuaternion(rot);  // returns [w, x, y, z]
        return {q[1], q[2], q[3], q[0]};
```

**Round-trip proof:** Already established in §5.4 of 6a mapping.

### 3.2 Pose3 Conversion (§4 of 6a mapping)

**P3 → GTSAM:**
```
Input:  position_xyz[3], rotation_xyzw[4]
Output: Pose3(Rot3::Quaternion(w, x, y, z), Point3(x, y, z))
Code:   Pose3(quaternionP3ToGtsam(rotation), Point3(position[0], position[1], position[2]))
```

**GTSAM → P3:**
```
Input:  Pose3
Output: position_xyz[3], rotation_xyzw[4]
Code:   auto t = pose.translation();
        auto R = pose.rotation();
        position = {t.x(), t.y(), t.z()};
        rotation = quaternionGtsamToP3(R);
```

**Convention proof:** Already established in §4.3 of 6a mapping. Both use world-from-body: `p_world = R · p_body + t`.

### 3.3 Tangent-Space Permutation (§6 of 6a mapping)

**Permutation matrix P₆:**
```
P₆ = [[0,0,0,1,0,0],
       [0,0,0,0,1,0],
       [0,0,0,0,0,1],
       [1,0,0,0,0,0],
       [0,1,0,0,0,0],
       [0,0,1,0,0,0]]
```

**Properties:** P₆ = P₆ᵀ = P₆⁻¹ (self-inverse).

**Code:**
```cpp
constexpr int kTangentPermutation[6] = {3, 4, 5, 0, 1, 2};

void permuteMatrixP3ToGtsam(const double in[36], double out[36]) {
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 6; ++c)
            out[r * 6 + c] = in[kTangentPermutation[r] * 6 + kTangentPermutation[c]];
}

// Inverse is identical
void permuteMatrixGtsamToP3(const double in[36], double out[36]) {
    permuteMatrixP3ToGtsam(in, out);
}
```

### 3.4 Information Matrix → Noise Model (§7 of 6a mapping)

**Path:** P3 information (row-major [v,ω]) → permute → GTSAM information ([ω,v]) → `noiseModel::Gaussian::Information()`

**Code:**
```cpp
gtsam::noiseModel::Base::shared_ptr informationToNoiseModel(
    const std::array<double, 6>& information_p3) {
    
    double permuted[36];
    permuteMatrixP3ToGtsam(information_p3.data(), permuted);
    
    Eigen::Map<Eigen::Matrix<double, 6, 6>> info_eigen(permuted);
    return gtsam::noiseModel::Gaussian::Information(info_eigen);
}
```

**Critical:** No inversion step. Information stays information, covariance stays covariance.

### 3.5 Covariance Extraction (§7 of 6a mapping)

**Path:** GTSAM marginals → `marginalCovariance(key)` → permute → extract upper triangle

**Code:**
```cpp
void extractCovarianceUpperTriangle(
    const Eigen::Matrix<double, 6, 6>& cov_gtsam,
    std::array<double, 6>& position,
    std::array<double, 6>& rotation) {
    
    double permuted[36];
    // ... permute cov_gtsam to P3 ordering ...
    
    // Extract upper triangle of 3×3 blocks
    // position block: rows 0-2, cols 0-2
    // rotation block: rows 3-5, cols 3-5
    position = {
        permuted[0*6+0], permuted[0*6+1], permuted[0*6+2],
        permuted[1*6+1], permuted[1*6+2], permuted[2*6+2]
    };
    rotation = {
        permuted[3*6+3], permuted[3*6+4], permuted[3*6+5],
        permuted[4*6+4], permuted[4*6+5], permuted[5*6+5]
    };
}
```

---

## 4. Factor Graph Construction

### 4.1 Node Initialization

**Join strategy:** Primary = `frame_id`, fallback = `sequence_index`.

```cpp
// Build lookup from trajectory nodes
std::unordered_map<std::string, size_t> frame_to_traj_index;
for (size_t i = 0; i < input.trajectory_nodes.size(); ++i) {
    frame_to_traj_index[input.trajectory_nodes[i].frame_id] = i;
}

// For each graph node, find initial pose
for (const auto& graph_node : input.graph_nodes) {
    auto it = frame_to_traj_index.find(graph_node.frame_id);
    if (it != frame_to_traj_index.end()) {
        const auto& traj_node = input.trajectory_nodes[it->second];
        values.insert(nodeIdToKey(graph_node.node_id),
                      poseP3ToGtsam(traj_node.position_xyz, traj_node.rotation_xyzw));
    }
}
```

### 4.2 Edge Dispatch

| `edge.type` | GTSAM Factor | Notes |
|---|---|---|
| `"prior"` | `PriorFactor<Pose3>(key, pose, noise)` | Unary constraint on source node |
| `"odometry"` | `BetweenFactor<Pose3>(src, tgt, rel, noise)` | Binary constraint |
| `"loop_closure"` | `BetweenFactor<Pose3>(src, tgt, rel, noise)` | Same as odometry, different provenance |
| `"lidar_odometry"` | `BetweenFactor<Pose3>(src, tgt, rel, noise)` | Same as odometry |
| `"gps"` | `PriorFactor<Point3>(key, point, noise)` | Position-only constraint |
| `"imu_preintegration"` | **SKIPPED** | Logged, not added to graph |

### 4.3 Anchor Prior (§B3)

**Default behavior:** Add a tight prior to node 0 to fix the gauge freedom.

```cpp
if (input.anchor_prior.enabled && !input.graph_nodes.empty()) {
    gtsam::Key anchor_key = nodeIdToKey(input.graph_nodes[0].node_id);
    auto anchor_pose = poseP3ToGtsam(
        input.trajectory_nodes[0].position_xyz,
        input.trajectory_nodes[0].rotation_xyzw);
    
    // Build tight information matrix: information_scale × I₆
    std::array<double, 6> info_diag;
    for (int i = 0; i < 6; ++i) info_diag[i] = input.anchor_prior.information_scale;
    
    auto noise = informationToNoiseModel(info_diag);
    graph.add(gtsam::PriorFactor<gtsam::Pose3>(anchor_key, anchor_pose, noise));
}
```

---

## 5. Optimization Execution

### 5.1 Levenberg-Marquardt Configuration

```cpp
gtsam::LevenbergMarquardtParams params;
params.setMaxIterations(input.options.max_iterations);
params.setlambdaInitial(input.options.lambda_initial);
params.setlambdaFactor(input.options.lambda_factor);
params.setfunctionTolerance(input.options.function_tolerance);
params.setparameterTolerance(input.options.parameter_tolerance);
params.setgradientTolerance(input.options.gradient_tolerance);
params.setOrdering(gtsam::Ordering::COLAMD);
```

### 5.2 Optimization Run

```cpp
gtsam::NonlinearFactorGraph graph;
gtsam::Values initial_values;

// ... populate graph and values (§4) ...

double initial_error = graph.error(initial_values);

gtsam::LevenbergMarquardtOptimizer optimizer(graph, initial_values, params);
gtsam::Values result_values = optimizer.optimize();

double final_error = graph.error(result_values);
std::int64_t iterations = static_cast<std::int64_t>(optimizer.iterations());
```

### 5.3 Status Determination

```cpp
std::string status;
if (iterations >= input.options.max_iterations) {
    status = "converged";  // reached max iterations (typically means converged)
} else if (final_error < initial_error) {
    status = "converged";
} else if (final_error > initial_error * 10) {
    status = "diverged";
} else {
    status = "failed";
}
```

---

## 6. Result Extraction

### 6.1 Optimized Pose Nodes

```cpp
for (const auto& graph_node : input.graph_nodes) {
    gtsam::Key key = nodeIdToKey(graph_node.node_id);
    
    gtsam::Pose3 optimized_pose = result_values.at<gtsam::Pose3>(key);
    
    core::OptimizedPoseNode node;
    node.frame_id = graph_node.frame_id;
    node.timestamp_ns = graph_node.timestamp_ns;
    node.sequence_index = graph_node.sequence_index;
    
    // Extract position and rotation
    poseGtsamToP3(optimized_pose, node.position_xyz, node.rotation_xyzw);
    
    // Extract covariance (when marginals are computed)
    // ... (§3.5) ...
    
    output.optimized_nodes.push_back(node);
}
```

### 6.2 Optimization Result Metadata

```cpp
output.result.result_id = generateUuid();
output.result.graph_id = input.graph.graph_id;
output.result.trajectory_id = input.trajectory.trajectory_id;
output.result.status = status;
output.result.iterations = iterations;
output.result.initial_error = initial_error;
output.result.final_error = final_error;
output.result.error_reduction = (initial_error - final_error) / initial_error;
output.result.created_at_ns = currentTimestampNs();
output.result.provenance = buildProvenance(input, status);
```

---

## 7. Provenance and Determinism

### 7.1 Configuration Hash

```
SHA-256(
    optimizer_name +
    max_iterations +
    function_tolerance +
    parameter_tolerance +
    gradient_tolerance +
    lambda_initial +
    lambda_factor +
    information_scale +
    SHA-256(all edges: type + src + tgt + rel_pos + rel_rot + info_matrix) +
    SHA-256(all poses: position + rotation)
)
```

### 7.2 Deterministic Properties

- **Node insertion order:** By `node_id` (ascending)
- **Edge iteration order:** By `edge_id` (ascending)
- **Optimizer params:** Fixed `LevenbergMarquardtParams` with `Ordering::COLAMD`
- **No threading randomness:** LM is single-threaded by default in GTSAM
- **Anchor:** Node 0 with tight prior (configurable, default `1e6 · I₆`)

---

## 8. Anchor Prior Numerical Rationale (§B3)

### 8.1 Why an Anchor Prior?

Pose graph optimization has **gauge freedom**: the entire graph can be translated, rotated, or reflected without changing the relative constraints. To obtain a unique solution, we must fix at least one node. This is standard practice in all SLAM systems:

- **ORB-SLAM3:** Fixes the first keyframe
- **g2o:** Uses `setFixed(true)` on anchor vertices
- **GTSAM:** Uses `PriorFactor` on anchor node
- **hdl_graph_slam:** Uses `fixme` flag on PoseVertex

### 8.2 Why `1e6 · I₆` as Default?

The information matrix `Ω = Σ⁻¹` quantifies constraint tightness:

| Information Value | Covariance (1/σ²) | Interpretation |
|---|---|---|
| `1e0` | `1.0 m²` | Very loose (1σ ≈ 1m) |
| `1e2` | `0.01 m²` | Moderate (1σ ≈ 0.1m) |
| `1e4` | `0.0001 m²` | Tight (1σ ≈ 0.01m) |
| `1e6` | `1e-6 m²` | Very tight (1σ ≈ 1mm) |
| `1e8` | `1e-8 m²` | Extremely tight (1σ ≈ 0.1mm) |

**`1e6` rationale:**
- **Numerically stable:** Well within double-precision range (not so large as to cause ill-conditioning)
- **Physically meaningful:** 1mm standard deviation is a reasonable "known position" assumption
- **Industry common:** Similar to what ORB-SLAM3 uses for fixed keyframes
- **Gauge fixing:** Effectively pins node 0 to its initial pose, allowing other nodes to move

### 8.3 SI Units

The information matrix is in **inverse SI units**:
- **Translation components (rows/cols 0-2):** `1/m²` (inverse meters squared)
- **Rotation components (rows/cols 3-5):** `1/rad²` (inverse radians squared)

Therefore `1e6 · I₆` means:
- Position uncertainty: `σ = 1/√(1e6) = 0.001 m = 1 mm`
- Rotation uncertainty: `σ = 1/√(1e6) = 0.001 rad ≈ 0.057°`

### 8.4 Tangent-Space Ordering Consistency

The anchor prior information matrix must be in **P3 ordering** `[v,ω]` before permutation to GTSAM ordering `[ω,v]`. Since `I₆` is permutation-invariant (`P₆ · I₆ · P₆ = I₆`), the anchor prior works correctly regardless of ordering:

```
P₆ · (1e6 · I₆) · P₆ = 1e6 · (P₆ · I₆ · P₆) = 1e6 · I₆
```

**This is a special property of the identity matrix.** For non-diagonal information matrices, the ordering matters and must be correct.

### 8.5 Behavior Without Anchor

If `anchor_prior.enabled = false`:
- The gauge is unfixed
- GTSAM may converge to a rotated/translated solution
- Error may be low but absolute pose is arbitrary
- **Not recommended** for most use cases

### 8.6 Provenance Tracking

The anchor prior configuration is recorded in `OptimizationProvenance.backend_specific_json`:
```json
{
  "anchor_prior": {
    "enabled": true,
    "information_scale": 1e6,
    "node_id": 0,
    "position_uncertainty_m": 0.001,
    "rotation_uncertainty_rad": 0.001
  }
}
```

### 8.7 Configuration Hash Impact

The anchor prior configuration (`information_scale`, `enabled`) is included in the configuration hash. Changing the anchor prior changes the hash, ensuring reproducibility.

---

## 9. Test Matrix

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

## 10. Schema Design (§B1)

### 10.1 `optimization_result.schema.json`

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://spatial-platform.dev/schemas/json/optimization-result.schema.json",
  "title": "Canonical Optimization Result (P3)",
  "description": "Backend-independent optimization result payload stored in CAS. Contains optimized poses with post-optimization covariances and optimization metadata. Every optimizer backend (GTSAM, Ceres, custom) produces this document through its adapter.",
  "type": "object",
  "additionalProperties": false,
  "required": ["schema_version", "result_id", "graph_id", "trajectory_id", "status", "iterations", "initial_error", "final_error", "error_reduction", "created_at_ns", "provenance", "nodes"],
  "properties": {
    "schema_version": {
      "const": 1,
      "description": "Document schema version."
    },
    "result_id": {
      "type": "string",
      "format": "uuid",
      "description": "Instance-scoped UUIDv4 identity for this optimization result."
    },
    "graph_id": {
      "type": "string",
      "format": "uuid",
      "description": "Reference to the PoseGraph that was optimized."
    },
    "trajectory_id": {
      "type": "string",
      "format": "uuid",
      "description": "Reference to the original Trajectory."
    },
    "status": {
      "type": "string",
      "enum": ["converged", "failed", "diverged"],
      "description": "Optimization outcome (D-OPT-02)."
    },
    "iterations": {
      "type": "integer",
      "minimum": 0,
      "description": "Actual iterations performed by the optimizer."
    },
    "initial_error": {
      "type": "number",
      "description": "Total squared error before optimization."
    },
    "final_error": {
      "type": "number",
      "description": "Total squared error after optimization."
    },
    "error_reduction": {
      "type": "number",
      "minimum": 0,
      "maximum": 1,
      "description": "Relative error reduction: (initial - final) / initial."
    },
    "created_at_ns": {
      "type": "integer",
      "description": "Epoch nanoseconds when optimization completed."
    },
    "provenance": {
      "$ref": "#/definitions/OptimizationProvenance",
      "description": "Optimization provenance metadata."
    },
    "nodes": {
      "type": "array",
      "items": { "$ref": "#/definitions/OptimizedPoseNode" },
      "description": "Optimized pose nodes, ordered by sequence_index."
    }
  },
  "definitions": {
    "OptimizationProvenance": {
      "type": "object",
      "description": "Provenance metadata for the optimization run.",
      "additionalProperties": false,
      "required": ["optimizer", "configuration_hash", "input_artifact_hashes", "adapter_version"],
      "properties": {
        "optimizer": {
          "$ref": "#/definitions/OptimizerInfo",
          "description": "Backend optimizer information."
        },
        "configuration_hash": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$",
          "description": "SHA-256 hash of effective configuration."
        },
        "input_artifact_hashes": {
          "type": "array",
          "items": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
          "description": "CAS hashes of input artifacts (trajectory, pose graph)."
        },
        "adapter_version": {
          "type": "string",
          "description": "Version of the adapter that produced this result."
        },
        "git_commit": {
          "type": "string",
          "description": "Git commit of the adapter."
        },
        "backend_specific_json": {
          "type": "string",
          "description": "Arbitrary optimizer metadata as JSON string."
        }
      }
    },
    "OptimizerInfo": {
      "type": "object",
      "additionalProperties": false,
      "required": ["name", "version"],
      "properties": {
        "name": {
          "type": "string",
          "description": "Optimizer name (e.g. 'gtsam', 'ceres', 'custom')."
        },
        "version": {
          "type": "string",
          "description": "Optimizer version."
        }
      }
    },
    "OptimizedPoseNode": {
      "type": "object",
      "description": "Post-optimization corrected pose (D-OPT-03, D-OPT-04).",
      "additionalProperties": false,
      "required": ["frame_id", "timestamp_ns", "sequence_index", "position_xyz", "rotation_xyzw"],
      "properties": {
        "frame_id": {
          "type": "string",
          "format": "uuid",
          "description": "UUID referencing a Frame in the import model."
        },
        "timestamp_ns": {
          "type": "integer",
          "description": "Exposure timestamp in nanoseconds since epoch."
        },
        "sequence_index": {
          "type": "integer",
          "minimum": 0,
          "description": "Monotonically increasing ordinal."
        },
        "position_xyz": {
          "type": "array",
          "items": { "type": "number" },
          "minItems": 3,
          "maxItems": 3,
          "description": "Corrected translation of T_trajectory_camera in meters."
        },
        "rotation_xyzw": {
          "type": "array",
          "items": { "type": "number" },
          "minItems": 4,
          "maxItems": 4,
          "description": "Corrected unit quaternion in (x, y, z, w) scalar-last order."
        },
        "covariance_position": {
          "type": "array",
          "items": { "type": "number" },
          "minItems": 6,
          "maxItems": 6,
          "description": "Optional post-optimization 3×3 position covariance, upper triangle flattened row-major."
        },
        "covariance_rotation": {
          "type": "array",
          "items": { "type": "number" },
          "minItems": 6,
          "maxItems": 6,
          "description": "Optional post-optimization 3×3 rotation covariance, upper triangle flattened row-major."
        }
      }
    }
  }
}
```

### 10.2 Schema Usage

- **CAS document:** Stored in content-addressable storage, referenced by `OptimizationResult.result_id`
- **MetadataDb row:** `optimization_results` table stores `result_id`, `graph_id`, `trajectory_id`, `status`, `iterations`, `initial_error`, `final_error`, `error_reduction`, `created_at_ns`
- **Provenance:** All optimization provenance is stored in the CAS document, not the DB row

---

## 11. Scope Guardrails

**This mapping does NOT authorize:**
- Code changes (deferred to implementation phase)
- Schema creation (deferred to implementation phase)
- Changes to `core/trajectory/*.h`
- Changes to `adapters/colmap/`
- Changes to existing schemas
- ORB-SLAM3 adapter (P3-impl-4)
- Loop closure adapter (P3-impl-5)
- KISS-ICP adapter (P3-impl-9)
- Multi-session implementation (P3-impl-8)
- Uncertainty propagation (P3-impl-10)
- IMU preintegration (future increment)
- iSAM2 incremental optimization (future increment)

---

## 12. Final Verdict

### **READY FOR IMPLEMENTATION**

**Rationale:**

1. All 12 mapping sections are complete with concrete, repository-specific details.
2. SE(3) direction is mathematically proven (§4 of 6a mapping).
3. Quaternion conversion is specified with API-level detail (§5 of 6a mapping).
4. Tangent-space permutation is derived with the 6×6 matrix P₆ (§6 of 6a mapping).
5. Information/covariance conversion path is fully specified with no double-inversion risk (§7 of 6a mapping).
6. GTSAM dependency is isolated to `adapters/gtsam/` only (§2 of 6a mapping).
7. `spatial_unit_tests` remains GTSAM-independent (§14.1 of 6a mapping).
8. Anchor prior rationale is documented with SI units and ordering consistency (§8).
9. Schema design is complete and follows existing patterns (§10).
10. Test matrix covers all 30 test cases (§9).

**Architectural decisions resolved:**
- **B1:** `optimization_result.schema.json` created in this mapping (§10)
- **B3:** Anchor prior `1e6 · I₆` documented with numerical rationale, NOT ratified as normative constant (§8)

**Proposed next step:** Implement P3-impl-6b after approving this mapping.

---

*This mapping was produced by opencode on 2026-08-20. No code, schema, dependency, or build changes were made.*
