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
#include "core/trajectory/optimizer.h"

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

// Implementation of the core TrajectoryOptimizer seam (D-7c-5 / §7.1) backed
// by this adapter. Translates the engine-facing canonical input into the
// adapter's OptimizationInput, runs optimize(), and maps the result back to
// the seam output. engine/ reaches GTSAM only through this class so no GTSAM
// type leaks past the boundary.
class GtsamTrajectoryOptimizer final : public core::TrajectoryOptimizer {
 public:
  core::PoseOptimizationOutput optimize(
      const core::PoseOptimizationInput& input) override {
    // Map seam input -> adapter input. A minimal Trajectory is reconstructed
    // from graph metadata for identity/lineage (optimize() only needs
    // trajectory_id for the result).
    OptimizationInput ai;
    ai.trajectory.trajectory_id = input.graph.trajectory_id;
    ai.trajectory.scene_id = input.graph.scene_id;
    ai.trajectory_nodes = input.initial_nodes;
    ai.graph = input.graph;
    ai.graph_nodes = input.graph_nodes;
    ai.graph_edges = input.graph_edges;
    ai.options = OptimizerOptions{};
    ai.anchor_prior.enabled = input.anchor_enabled;
    ai.anchor_prior.information_scale = input.anchor_information_scale;

    OptimizationOutput ao;
    spatial::adapters::gtsam::optimize(ai, ao);

    // Map adapter output -> seam output.
    core::PoseOptimizationOutput out;
    out.result_id = ao.result.result_id;
    out.trace.converged = ao.result.status == "converged";
    out.trace.iterations = ao.result.iterations;
    out.trace.initial_error = ao.result.initial_error;
    out.trace.final_error = ao.result.final_error;
    out.trace.error_reduction = ao.result.error_reduction;
    out.trace.status = ao.result.status;
    out.optimized_nodes.reserve(ao.optimized_nodes.size());
    for (const auto& n : ao.optimized_nodes) {
      core::TrajectoryPoseNode tn;
      tn.frame_id = n.frame_id;
      tn.timestamp_ns = n.timestamp_ns;
      tn.sequence_index = n.sequence_index;
      tn.position_xyz = n.position_xyz;
      tn.rotation_xyzw = n.rotation_xyzw;
      tn.covariance_position = n.covariance_position;
      tn.covariance_rotation = n.covariance_rotation;
      out.optimized_nodes.push_back(std::move(tn));
    }
    return out;
  }
};

}  // namespace spatial::adapters::gtsam
