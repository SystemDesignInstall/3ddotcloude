// P3-impl-7 Phase 3 — LoopClosure -> PoseGraph stage (P3-impl-7c §7.2).
//
// The stage resolves the closure's source/target frames onto the trajectory
// pose nodes, computes the D2 spatial_separation_m diagnostic, checks metric
// eligibility (D4 / INV-3), and — when eligible and the measurement is real —
// emits exactly one metric PoseGraphEdge. No GTSAM here.

#include "engine/pipeline/loop_closure_to_pose_graph.h"

#include <cstdint>

#include "core/geometry/direction_math.h"
#include "core/trajectory/pose_graph_helpers.h"

namespace spatial::engine {

namespace {

// Euclidean separation of the trajectory pose nodes of the source and target
// frames: ||p_t_ww - p_s_ww|| in metres (D-LC-06 / D2). A pure diagnostic —
// direction-agnostic, never the edge translation, never ||relative_position_xyz||
// (P3-impl-7c §6). Returns 0.0 when either frame is unresolved or positions are
// degenerate.
double ComputeSpatialSeparationM(
    const std::vector<spatial::core::TrajectoryPoseNode>& nodes,
    const std::string& source_frame_id, const std::string& target_frame_id) {
  const spatial::core::TrajectoryPoseNode* source = nullptr;
  const spatial::core::TrajectoryPoseNode* target = nullptr;
  for (const auto& n : nodes) {
    if (n.frame_id == source_frame_id) source = &n;
    if (n.frame_id == target_frame_id) target = &n;
  }
  if (source == nullptr || target == nullptr) return 0.0;
  if (source_frame_id == target_frame_id) return 0.0;
  const double dx = target->position_xyz[0] - source->position_xyz[0];
  const double dy = target->position_xyz[1] - source->position_xyz[1];
  const double dz = target->position_xyz[2] - source->position_xyz[2];
  return spatial::core::geometry::Norm3({dx, dy, dz});
}

}  // namespace

LoopClosureToPoseGraphResult LoopClosureToPoseGraph(
    const LoopClosureToPoseGraphInput& input) {
  LoopClosureToPoseGraphResult result;
  result.closure = input.closure;

  const auto* nodes = input.trajectory_nodes;
  if (nodes == nullptr) {
    result.closure.spatial_separation_m = 0.0;
    return result;
  }

  // D2: fill the spatial_separation_m diagnostic from the trajectory nodes.
  const double spatial = ComputeSpatialSeparationM(
      *nodes, input.closure.source_frame_id, input.closure.target_frame_id);
  result.spatial_separation_m = spatial;
  result.closure.spatial_separation_m = spatial;

  // INV-3 gate: metric eligibility is provenance-derived (D4). Undeclared /
  // by-fiat basis => NO metric edge, even with a valid measured relative pose.
  result.metric_eligible =
      spatial::core::MetricEligibleTrajectoryBasis(input.metric_basis);
  if (!result.metric_eligible) return result;

  if (input.closure.status != "accepted") return result;
  if (input.closure.source_frame_id == input.closure.target_frame_id) {
    return result;
  }

  // Leave the accumulation of the edge-id to the caller via a dedicated edge;
  // we emit a single loop-closure edge with edge_id 0 resolved later if an
  // env/param override is needed; otherwise the caller re-stamps it. Build the
  // measurement from the closure's D5 resolved metric pose.
  spatial::core::MetricLoopClosureMeasurement measurement;
  measurement.position_cs = input.closure.relative_position_xyz;
  measurement.rotation_cst = input.closure.relative_rotation_xyzw;
  measurement.geometric_residual = input.closure.geometric_residual;

  result.metric_edge = spatial::core::BuildMetricLoopClosureEdge(
      input.closure, *nodes, measurement, 0, input.configuration_hash,
      input.metric_basis);
  return result;
}

}  // namespace spatial::engine
