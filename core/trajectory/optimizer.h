#pragma once

// Canonical Pose-Graph Optimization SEAM (P3 D-7c-5 / P3-impl-7c §7.1).
// The single engine-facing interface for running pose-graph optimization.
// No optimizer (GTSAM/Ceres/custom) types appear here; every optimizer backend
// is reached behind this interface (D-AB-02). This header is header-only and
// carries no backend headers.
//
// Naming: the seam input/output types are named PoseOptimizationInput /
// PoseOptimizationOutput to avoid colliding with backend adapter
// `OptimizationInput`/`OptimizationOutput` types that share the phrase in
// their own namespaces. The interface is named TrajectoryOptimizer per
// P3-impl-7c §7.1.

#include <cstdint>
#include <string>
#include <vector>

#include "core/trajectory/optimization.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/trajectory.h"

namespace spatial::core {

// Input for one optimization run, expressed in canonical pose-graph types.
struct PoseOptimizationInput {
  PoseGraph graph;                      // graph metadata (graph_id, trajectory_id)
  std::vector<PoseGraphNode> graph_nodes;   // graph nodes, node_id -> frame_id
  std::vector<PoseGraphEdge> graph_edges;   // odometry + metric loop closures
  std::vector<TrajectoryPoseNode> initial_nodes;  // initial values
  // Anchor prior on node 0 (behaviour and defaults mirror the GTSAM adapter's
  // AnchorPriorConfig; engine passes values through unchanged).
  double anchor_information_scale = 1e6;
  bool anchor_enabled = true;
};

// Per-run optimizer telemetry (diagnostic; D-OPT-02 semantics).
struct OptimizationTrace {
  bool converged = false;               // status == "converged"
  std::int64_t iterations = 0;
  double initial_error = 0.0;
  double final_error = 0.0;
  double error_reduction = 0.0;         // (initial - final) / initial, in [0,1]
  std::string status;                   // "converged" | "failed" | "diverged"
};

// Result of one optimization run through the seam.
struct PoseOptimizationOutput {
  std::vector<TrajectoryPoseNode> optimized_nodes;  // corrected poses (T_trajectory_camera)
  OptimizationTrace trace;
  std::string result_id;                // optimizer result id (DB lineage)
};

// Backend-neutral optimizer seam. engine/ dispatches exclusively through this
// interface; each backend (currently GTSAM) supplies an implementation behind
// it. A backend that produces no valid solution returns output with
// status="failed"/converged=false; the optimized_nodes are then the initial
// (diagnostic) state and must NOT be consumed as an optimized solution.
class TrajectoryOptimizer {
 public:
  virtual ~TrajectoryOptimizer() = default;
  virtual PoseOptimizationOutput optimize(const PoseOptimizationInput&) = 0;
};

}  // namespace spatial::core
