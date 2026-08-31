#pragma once

// P3-impl-6b: GTSAM Levenberg-Marquardt optimizer adapter.
// This header is the sole public surface of the GTSAM adapter.
//
// Architecture boundary:
// - This header includes core/trajectory types (P3 canonical types).
// - core/trajectory/*.h must NOT include any GTSAM headers.
// - The adapter converts P3 ↔ GTSAM at the boundary.
// - No GTSAM types leak into core/ or any other subsystem.
//
// Pose convention: T_trajectory_camera (world-from-body), ADR-007.
// Quaternion order: (x, y, z, w) scalar-last, ADR-007.
// Tangent space: [v, ω] in P3, [ω, v] in GTSAM — permuted at boundary.

#include <cstdint>
#include <string>
#include <vector>

#include "core/trajectory/trajectory.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/optimization.h"

namespace spatial::adapters::gtsam {

// Levenberg-Marquardt optimizer configuration.
struct OptimizerOptions {
  std::int64_t max_iterations = 100;
  double lambda_initial = 1e-4;
  double lambda_factor = 10.0;
  double function_tolerance = 1e-10;
  double parameter_tolerance = 1e-10;
  double gradient_tolerance = 1e-10;
};

// Anchor prior configuration (§B3 of implementation mapping).
// The default information_scale is a configurable default, NOT a normative
// architectural constant. It may be overridden per-optimization-run.
struct AnchorPriorConfig {
  double information_scale = 1e6;  // scaling factor for I₆ prior
  bool enabled = true;             // whether to anchor node 0
};

// Complete input for one optimization run.
struct OptimizationInput {
  core::Trajectory trajectory;
  std::vector<core::TrajectoryPoseNode> trajectory_nodes;
  core::PoseGraph graph;
  std::vector<core::PoseGraphNode> graph_nodes;
  std::vector<core::PoseGraphEdge> graph_edges;
  OptimizerOptions options;
  AnchorPriorConfig anchor_prior;
};

// Result of one optimization run.
struct OptimizationOutput {
  core::OptimizationResult result;
  std::vector<core::OptimizedPoseNode> optimized_nodes;
};

// Main entry point: converts P3 types → GTSAM, runs Levenberg-Marquardt,
// extracts canonical result. Returns false on hard failure (empty graph,
// exception); result.status indicates converged / failed / diverged.
bool optimize(OptimizationInput input, OptimizationOutput& output);

}  // namespace spatial::adapters::gtsam
