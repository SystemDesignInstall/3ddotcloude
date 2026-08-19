#pragma once

// Canonical Optimization domain types (P3).
// Backend-independent representation of pose graph optimization results.
// Every optimizer backend (GTSAM, Ceres, custom) produces these types
// through its adapter. The types carry no backend-specific fields.
//
// Normative decisions: D-OPT-01 through D-OPT-06 (P3-trajectory-pose-graph-loop-closure.md).
// Pose convention: corrected poses use T_trajectory_camera.
// Quaternion order: (x, y, z, w) scalar-last per ADR-007.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/reconstruction/reconstruction.h"

namespace spatial::core {

// --- Optimization Provenance ---

struct OptimizationProvenance {
  struct OptimizerInfo {
    std::string name;                      // "gtsam", "ceres", "custom", etc.
    std::string version;
    bool operator==(const OptimizerInfo&) const = default;
  };
  OptimizerInfo optimizer;
  std::string configuration_hash;          // SHA-256 of effective configuration
  std::vector<std::string> input_artifact_hashes;  // CAS hashes of input artifacts
  std::string adapter_version;
  std::string git_commit;
  std::string backend_specific_json;       // arbitrary optimizer metadata
  bool operator==(const OptimizationProvenance&) const = default;
};

// --- Optimized Pose Node (CAS payload, D-OPT-03, D-OPT-04) ---
// Post-optimization corrected pose. Stored in a separate CAS document
// from the original trajectory (D-OPT-05).

struct OptimizedPoseNode {
  std::string frame_id;                    // UUID string -> Frame
  std::int64_t timestamp_ns = 0;
  std::int64_t sequence_index = 0;
  std::array<double, 3> position_xyz{};    // corrected position
  std::array<double, 4> rotation_xyzw{};   // corrected rotation (x,y,z,w)
  std::array<double, 6> covariance_position{};  // post-optimization covariance
  std::array<double, 6> covariance_rotation{};
  bool operator==(const OptimizedPoseNode&) const = default;
};

// --- Optimization Result (root entity, D-OPT-01, D-OPT-02) ---

struct OptimizationResult {
  std::string result_id;                   // UUIDv4
  std::string graph_id;                    // -> PoseGraph.graph_id
  std::string trajectory_id;               // -> Trajectory.trajectory_id
  std::string status;                      // "converged" | "failed" | "diverged" (D-OPT-02)
  std::int64_t iterations = 0;             // actual iterations performed
  double initial_error = 0.0;              // total squared error before optimization
  double final_error = 0.0;                // total squared error after optimization
  double error_reduction = 0.0;            // (initial - final) / initial, in [0, 1]
  std::int64_t created_at_ns = 0;
  OptimizationProvenance provenance;
  bool operator==(const OptimizationResult&) const = default;
};

}  // namespace spatial::core
