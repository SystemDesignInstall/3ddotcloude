// P3-impl-6b: GTSAM Levenberg-Marquardt optimizer adapter.
// This is the sole compile-unit that includes GTSAM headers in the
// spatial-platform codebase. It converts P3 canonical types → GTSAM types
// at the adapter boundary and runs batch Levenberg-Marquardt optimization.
//
// Architecture boundary: this file links spatial_core (PUBLIC) and
// gtsam::gtsam (PRIVATE). No GTSAM types leak into core/.
//
// Pose convention: T_trajectory_camera (world-from-body), ADR-007.
// Quaternion order: (x, y, z, w) scalar-last, ADR-007.
// Tangent space: [v, ω] in P3, [ω, v] in GTSAM.

#include "adapters/gtsam/gtsam_optimizer_adapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtsam/geometry/Pose3.h>
#include <gtsam/geometry/Point3.h>
#include <gtsam/geometry/Rot3.h>
#include <gtsam/linear/NoiseModel.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/nonlinear/LevenbergMarquardtParams.h>
#include <gtsam/nonlinear/Marginals.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/slam/BetweenFactor.h>

#include <Eigen/Core>

#include "core/trajectory/trajectory.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/optimization.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"

namespace spatial::adapters::gtsam {

// ============================================================================
// Internal conversion functions
// ============================================================================

namespace {

// --- Tangent-space permutation matrix P6 ---
constexpr int kTangentPermutation[6] = {3, 4, 5, 0, 1, 2};

void permuteMatrix6x6(const double in[36], double out[36]) {
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c)
      out[r * 6 + c] = in[kTangentPermutation[r] * 6 + kTangentPermutation[c]];
}

// P3 (x,y,z,w) scalar-last → GTSAM Rot3 (w,x,y,z) scalar-first
::gtsam::Rot3 quaternionP3ToGtsam(const std::array<double, 4>& xyzw) {
  return ::gtsam::Rot3(xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
}

// GTSAM Rot3 → P3 (x,y,z,w) scalar-last
std::array<double, 4> quaternionGtsamToP3(const ::gtsam::Rot3& rot) {
  auto q = rot.toQuaternion();
  return {q.x(), q.y(), q.z(), q.w()};
}

// P3 position + rotation → GTSAM Pose3
::gtsam::Pose3 poseP3ToGtsam(const std::array<double, 3>& position,
                            const std::array<double, 4>& rotation) {
  return ::gtsam::Pose3(
      quaternionP3ToGtsam(rotation),
      ::gtsam::Point3(position[0], position[1], position[2]));
}

// GTSAM Pose3 → P3 position + rotation
void poseGtsamToP3(const ::gtsam::Pose3& pose,
                   std::array<double, 3>& position,
                   std::array<double, 4>& rotation) {
  auto t = pose.translation();
  position = {t.x(), t.y(), t.z()};
  rotation = quaternionGtsamToP3(pose.rotation());
}

// Information matrix → GTSAM noise model
::gtsam::noiseModel::Base::shared_ptr informationToNoiseModel(
    const std::array<double, 36>& information_p3) {
  double permuted[36];
  permuteMatrix6x6(information_p3.data(), permuted);
  Eigen::Map<Eigen::Matrix<double, 6, 6>> info_eigen(permuted);
  return ::gtsam::noiseModel::Gaussian::Information(info_eigen);
}

// Build diagonal information matrix from 6-vector
std::array<double, 36> diagonalInformation(const std::array<double, 6>& diag) {
  std::array<double, 36> mat{};
  for (int i = 0; i < 6; ++i)
    mat[i * 6 + i] = diag[i];
  return mat;
}

// Extract covariance upper triangle from 6Г—6 in GTSAM ordering
void extractCovarianceUpperTriangle(
    const Eigen::Matrix<double, 6, 6>& cov_gtsam,
    std::array<double, 6>& position,
    std::array<double, 6>& rotation) {
  Eigen::Matrix<double, 6, 6> cov_p3;
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c)
      cov_p3(r, c) = cov_gtsam(kTangentPermutation[r], kTangentPermutation[c]);
  position = {
      cov_p3(0, 0), cov_p3(0, 1), cov_p3(0, 2),
      cov_p3(1, 1), cov_p3(1, 2), cov_p3(2, 2)};
  rotation = {
      cov_p3(3, 3), cov_p3(3, 4), cov_p3(3, 5),
      cov_p3(4, 4), cov_p3(4, 5), cov_p3(5, 5)};
}

::gtsam::Key nodeIdToKey(std::int64_t node_id) {
  return static_cast<::gtsam::Key>(node_id);
}

std::string generateResultId() {
  return core::FormatUuid(core::GenerateUuid());
}

std::string computeConfigurationHash(const OptimizationInput& input) {
  core::Sha256 hasher;
  hasher.Update(std::to_string(input.options.max_iterations));
  hasher.Update(std::to_string(input.options.function_tolerance));
  hasher.Update(std::to_string(input.options.parameter_tolerance));
  hasher.Update(std::to_string(input.options.gradient_tolerance));
  hasher.Update(std::to_string(input.options.lambda_initial));
  hasher.Update(std::to_string(input.options.lambda_factor));
  hasher.Update(std::to_string(input.anchor_prior.information_scale));
  hasher.Update(input.anchor_prior.enabled ? "1" : "0");

  auto sorted_edges = input.graph_edges;
  std::sort(sorted_edges.begin(), sorted_edges.end(),
            [](const core::PoseGraphEdge& a, const core::PoseGraphEdge& b) {
              return a.edge_id < b.edge_id;
            });
  for (const auto& edge : sorted_edges) {
    hasher.Update(edge.type);
    hasher.Update(std::to_string(edge.source_node_id));
    hasher.Update(std::to_string(edge.target_node_id));
    for (double v : edge.relative_position_xyz) hasher.Update(std::to_string(v));
    for (double v : edge.relative_rotation_xyzw) hasher.Update(std::to_string(v));
    for (double v : edge.information_matrix_6x6) hasher.Update(std::to_string(v));
  }

  auto sorted_nodes = input.graph_nodes;
  std::sort(sorted_nodes.begin(), sorted_nodes.end(),
            [](const core::PoseGraphNode& a, const core::PoseGraphNode& b) {
              return a.node_id < b.node_id;
            });
  for (const auto& node : sorted_nodes) {
    hasher.Update(std::to_string(node.node_id));
    for (const auto& tp : input.trajectory_nodes) {
      if (tp.frame_id == node.frame_id) {
        for (double v : tp.position_xyz) hasher.Update(std::to_string(v));
        for (double v : tp.rotation_xyzw) hasher.Update(std::to_string(v));
        break;
      }
    }
  }
  return core::Sha256Hex(hasher.Final());
}

core::OptimizationProvenance buildProvenance(
    const OptimizationInput& input,
    const std::string& config_hash,
    const std::string& status,
    std::int64_t iterations) {
  core::OptimizationProvenance prov;
  prov.optimizer.name = "gtsam";
  prov.optimizer.version = "4.2.1";
  prov.configuration_hash = config_hash;
  prov.adapter_version = "0.1.0";
  prov.git_commit = "unknown";

  std::ostringstream oss;
  oss << "{\"optimizer\":\"LevenbergMarquardt\""
      << ",\"max_iterations\":" << input.options.max_iterations
      << ",\"lambda_initial\":" << input.options.lambda_initial
      << ",\"lambda_factor\":" << input.options.lambda_factor
      << ",\"function_tolerance\":" << input.options.function_tolerance
      << ",\"anchor_prior\":{\"enabled\":" << (input.anchor_prior.enabled ? "true" : "false")
      << ",\"information_scale\":" << input.anchor_prior.information_scale
      << "}"
      << ",\"status\":\"" << status << "\""
      << ",\"iterations\":" << iterations
      << "}";
  prov.backend_specific_json = oss.str();
  return prov;
}

}  // anonymous namespace

// ============================================================================
// Main optimization entry point
// ============================================================================

bool optimize(OptimizationInput input, OptimizationOutput& output) {
  if (input.graph_nodes.empty() || input.graph_edges.empty()) {
    // Semantics: status="failed" means the optimizer produced NO valid
    // solution. The optimized_nodes populated below are the INITIAL / diagnostic
    // trajectory state copied from the input; they are NOT an optimized
    // solution and must not be consumed as such by callers.
    output.result.status = "failed";
    output.result.iterations = 0;
    output.result.initial_error = 0.0;
    output.result.final_error = 0.0;
    output.result.error_reduction = 0.0;
    output.result.result_id = generateResultId();
    output.result.graph_id = input.graph.graph_id;
    output.result.trajectory_id = input.trajectory.trajectory_id;
    output.result.created_at_ns = 0;
    output.result.provenance = buildProvenance(
        input, computeConfigurationHash(input), "failed", 0);
    std::unordered_map<std::string, size_t> frame_to_traj;
    for (size_t i = 0; i < input.trajectory_nodes.size(); ++i)
      frame_to_traj[input.trajectory_nodes[i].frame_id] = i;
    for (const auto& gn : input.graph_nodes) {
      core::OptimizedPoseNode node;
      node.frame_id = gn.frame_id;
      node.timestamp_ns = gn.timestamp_ns;
      node.sequence_index = gn.node_id;
      auto it = frame_to_traj.find(gn.frame_id);
      if (it != frame_to_traj.end()) {
        const auto& tp = input.trajectory_nodes[it->second];
        node.position_xyz = tp.position_xyz;
        node.rotation_xyzw = tp.rotation_xyzw;
      } else {
        node.position_xyz = {0.0, 0.0, 0.0};
        node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
      }
      output.optimized_nodes.push_back(node);
    }
    return true;
  }

  std::unordered_map<std::string, size_t> frame_to_traj_index;
  for (size_t i = 0; i < input.trajectory_nodes.size(); ++i)
    frame_to_traj_index[input.trajectory_nodes[i].frame_id] = i;

  std::unordered_map<std::int64_t, size_t> nodeid_to_graph_index;
  for (size_t i = 0; i < input.graph_nodes.size(); ++i)
    nodeid_to_graph_index[input.graph_nodes[i].node_id] = i;

  ::gtsam::NonlinearFactorGraph graph;
  ::gtsam::Values initial_values;

  for (const auto& gn : input.graph_nodes) {
    auto it = frame_to_traj_index.find(gn.frame_id);
    if (it != frame_to_traj_index.end()) {
      const auto& tp = input.trajectory_nodes[it->second];
      initial_values.insert(nodeIdToKey(gn.node_id),
                            poseP3ToGtsam(tp.position_xyz, tp.rotation_xyzw));
    } else {
      initial_values.insert(nodeIdToKey(gn.node_id), ::gtsam::Pose3());
    }
  }

  if (input.anchor_prior.enabled && !input.graph_nodes.empty()) {
    ::gtsam::Key anchor_key = nodeIdToKey(input.graph_nodes[0].node_id);
    ::gtsam::Pose3 anchor_pose;
    auto ait = frame_to_traj_index.find(input.graph_nodes[0].frame_id);
    if (ait != frame_to_traj_index.end()) {
      const auto& tp = input.trajectory_nodes[ait->second];
      anchor_pose = poseP3ToGtsam(tp.position_xyz, tp.rotation_xyzw);
    }
    std::array<double, 6> info_diag;
    for (int i = 0; i < 6; ++i)
      info_diag[i] = input.anchor_prior.information_scale;
    auto noise = informationToNoiseModel(diagonalInformation(info_diag));
    graph.add(::gtsam::PriorFactor<::gtsam::Pose3>(anchor_key, anchor_pose, noise));
  }

  for (const auto& edge : input.graph_edges) {
    auto src_it = nodeid_to_graph_index.find(edge.source_node_id);
    auto tgt_it = nodeid_to_graph_index.find(edge.target_node_id);

    if (src_it == nodeid_to_graph_index.end() ||
        (edge.type != "prior" && tgt_it == nodeid_to_graph_index.end()))
      continue;

    auto noise = informationToNoiseModel(edge.information_matrix_6x6);

    if (edge.type == "prior") {
      ::gtsam::Key key = nodeIdToKey(edge.source_node_id);
      ::gtsam::Pose3 prior_pose = poseP3ToGtsam(
          edge.relative_position_xyz, edge.relative_rotation_xyzw);
      graph.add(::gtsam::PriorFactor<::gtsam::Pose3>(key, prior_pose, noise));

    } else if (edge.type == "odometry" || edge.type == "loop_closure" ||
               edge.type == "lidar_odometry") {
      ::gtsam::Key src_key = nodeIdToKey(edge.source_node_id);
      ::gtsam::Key tgt_key = nodeIdToKey(edge.target_node_id);
      ::gtsam::Pose3 relative = poseP3ToGtsam(
          edge.relative_position_xyz, edge.relative_rotation_xyzw);
      graph.add(::gtsam::BetweenFactor<::gtsam::Pose3>(
          src_key, tgt_key, relative, noise));

    } else if (edge.type == "gps") {
      // GNSS is a POSITION-ONLY measurement. Build the prior pose with the
      // trajectory's own rotation estimate so the PriorFactor<Pose3> rotation
      // residual is ~zero and GNSS never injects independent orientation info.
      ::gtsam::Key key = nodeIdToKey(edge.source_node_id);
      ::gtsam::Pose3 gps_pose;
      auto git = frame_to_traj_index.find(
          input.graph_nodes[nodeid_to_graph_index[edge.source_node_id]].frame_id);
      if (git != frame_to_traj_index.end())
        gps_pose = poseP3ToGtsam(
            input.trajectory_nodes[git->second].position_xyz,
            input.trajectory_nodes[git->second].rotation_xyzw);
      auto gps_pos = ::gtsam::Point3(
          edge.relative_position_xyz[0],
          edge.relative_position_xyz[1],
          edge.relative_position_xyz[2]);
      gps_pose = ::gtsam::Pose3(gps_pose.rotation(), gps_pos);
      auto noise = informationToNoiseModel(edge.information_matrix_6x6);
      graph.add(::gtsam::PriorFactor<::gtsam::Pose3>(key, gps_pose, noise));
    }
  }

  if (graph.size() == 0) {
    output.result.status = "failed";
    output.result.iterations = 0;
    output.result.initial_error = 0.0;
    output.result.final_error = 0.0;
    output.result.error_reduction = 0.0;
    output.result.result_id = generateResultId();
    output.result.graph_id = input.graph.graph_id;
    output.result.trajectory_id = input.trajectory.trajectory_id;
    output.result.created_at_ns = 0;
    output.result.provenance = buildProvenance(
        input, computeConfigurationHash(input), "failed", 0);
    for (const auto& gn : input.graph_nodes) {
      core::OptimizedPoseNode node;
      node.frame_id = gn.frame_id;
      node.timestamp_ns = gn.timestamp_ns;
      node.sequence_index = gn.node_id;
      auto it = frame_to_traj_index.find(gn.frame_id);
      if (it != frame_to_traj_index.end()) {
        const auto& tp = input.trajectory_nodes[it->second];
        node.position_xyz = tp.position_xyz;
        node.rotation_xyzw = tp.rotation_xyzw;
      } else {
        node.position_xyz = {0.0, 0.0, 0.0};
        node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
      }
      output.optimized_nodes.push_back(node);
    }
    return true;
  }

  ::gtsam::LevenbergMarquardtParams params;
  params.maxIterations = static_cast<size_t>(input.options.max_iterations);
  params.relativeErrorTol = input.options.function_tolerance;
  params.absoluteErrorTol = input.options.parameter_tolerance;
  params.lambdaInitial = input.options.lambda_initial;
  params.lambdaFactor = input.options.lambda_factor;
  params.orderingType = ::gtsam::Ordering::COLAMD;

  double initial_error = 0.0;
  try {
    initial_error = graph.error(initial_values);
  } catch (...) {
    output.result.status = "failed";
    output.result.iterations = 0;
    output.result.initial_error = 0.0;
    output.result.final_error = 0.0;
    output.result.error_reduction = 0.0;
    return true;
  }

  ::gtsam::Values result_values;
  std::int64_t iterations = 0;
  std::string status = "converged";

  try {
    ::gtsam::LevenbergMarquardtOptimizer optimizer(graph, initial_values, params);
    result_values = optimizer.optimize();
    iterations = static_cast<std::int64_t>(optimizer.iterations());
  } catch (...) {
    status = "failed";
    iterations = 0;
  }

  double final_error = 0.0;
  try {
    final_error = graph.error(result_values);
  } catch (...) {
    status = "failed";
  }

  if (status != "failed") {
    if (final_error > initial_error * 10.0 && initial_error > 0.0)
      status = "diverged";
    else
      status = "converged";
  }

  std::unique_ptr<::gtsam::Marginals> marginals;
  try {
    marginals = std::make_unique<::gtsam::Marginals>(
        graph, result_values, ::gtsam::Marginals::CHOLESKY);
  } catch (...) {
    marginals.reset();
  }

  std::vector<core::OptimizedPoseNode> optimized_nodes;
  for (const auto& gn : input.graph_nodes) {
    ::gtsam::Key key = nodeIdToKey(gn.node_id);
    core::OptimizedPoseNode node;
    node.frame_id = gn.frame_id;
    node.timestamp_ns = gn.timestamp_ns;
    node.sequence_index = gn.node_id;

    try {
      ::gtsam::Pose3 optimized_pose = result_values.at<::gtsam::Pose3>(key);
      poseGtsamToP3(optimized_pose, node.position_xyz, node.rotation_xyzw);
    } catch (...) {
      node.position_xyz = {0.0, 0.0, 0.0};
      node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
    }

    if (marginals) {
      try {
        Eigen::Matrix<double, 6, 6> cov = marginals->marginalCovariance(key);
        extractCovarianceUpperTriangle(cov, node.covariance_position, node.covariance_rotation);
      } catch (...) {
        node.covariance_position = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        node.covariance_rotation = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
      }
    }
    optimized_nodes.push_back(node);
  }

  std::string config_hash = computeConfigurationHash(input);

  output.result.result_id = generateResultId();
  output.result.graph_id = input.graph.graph_id;
  output.result.trajectory_id = input.trajectory.trajectory_id;
  output.result.status = status;
  output.result.iterations = iterations;
  output.result.initial_error = initial_error;
  output.result.final_error = final_error;
  output.result.error_reduction =
      (initial_error > 0.0) ? (initial_error - final_error) / initial_error : 0.0;
  output.result.created_at_ns = 0;
  output.result.provenance = buildProvenance(input, config_hash, status, iterations);
  output.optimized_nodes = std::move(optimized_nodes);

  return true;
}

}  // namespace spatial::adapters::gtsam
