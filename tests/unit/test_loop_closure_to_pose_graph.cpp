// P3-impl-7 Phase 3 — LoopClosure -> PoseGraph stage tests (P3-impl-7c §7.2).
//
// Covers:
//   T4  — accepted closure + declared metric basis + real measurement
//         => exactly ONE metric loop_closure edge, spatial_separation_m filled.
//   T5  — spatial_separation_m (D2) == ||p_t_ww - p_s_ww|| and is demonstrably
//         different from the edge translation relative_position_xyz.
//   INV-3 — undeclared / by-fiat metric basis => NO edge (verified-visual-only),
//         even though the closure is accepted and carries a relative pose.
//   INV-1 — no metric measurement (zero position) => no edge.
//   D2 guard — unresolved frames => spatial_separation_m == 0 and no edge.
// D4 validate — ValidateMetricBasis rejects bare declared:true.

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/pose_graph_helpers.h"
#include "core/trajectory/trajectory.h"
#include "engine/pipeline/loop_closure_to_pose_graph.h"

namespace {

using spatial::core::LoopClosure;
using spatial::core::MetricBasis;
using spatial::core::MetricBasisSource;
using spatial::core::MetricBasisType;
using spatial::core::PoseGraphEdge;
using spatial::core::TrajectoryPoseNode;
using spatial::engine::LoopClosureToPoseGraph;
using spatial::engine::LoopClosureToPoseGraphInput;
using spatial::engine::LoopClosureToPoseGraphResult;

TrajectoryPoseNode MakeNode(std::int64_t seq, const std::string& frame_id,
                            double x, double y, double z) {
  TrajectoryPoseNode n;
  n.frame_id = frame_id;
  n.sequence_index = seq;
  n.position_xyz = {x, y, z};
  n.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  return n;
}

MetricBasis MakeBasis() {
  MetricBasis mb;
  mb.declared = true;
  mb.source = MetricBasisSource::kTrajectory;
  mb.basis = MetricBasisType::kCalibratedBaseline;
  mb.provenance.configuration_hash = "deadbeef00000000000000000000000000";
  mb.scale_calibration_ref = "cas://calib/baseline_v1";
  return mb;
}

// An accepted closure from node[4] (drifted) revisiting node[0] (origin), with
// a resolved metric relative pose (D5).
LoopClosure MakeAcceptedRevisit(const std::vector<TrajectoryPoseNode>& nodes,
                                const std::array<double, 3>& rel_pos = {-0.4, -0.5, 0.0}) {
  LoopClosure lc;
  lc.status = "accepted";
  lc.source_frame_id = nodes[4].frame_id;
  lc.target_frame_id = nodes[0].frame_id;
  lc.inlier_ratio = 0.9;
  lc.inlier_count = 30;
  lc.confidence = 0.95;
  lc.has_relative_pose = true;
  lc.relative_position_xyz = rel_pos;
  lc.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  lc.geometric_residual = 0.5;
  return lc;
}

// Drifted closed square (5 nodes): node 0 at origin, node 4 drifted from
// (-0.12,-0.16,0) to (0.12,0.16,0) -> so ||p4 - p0|| = 0.2 in W.
std::vector<TrajectoryPoseNode> DriftedSquare() {
  std::vector<TrajectoryPoseNode> nodes;
  nodes.push_back(MakeNode(0, "00000000-0000-0000-0000-0000000000F0", 0.0, 0.0, 0.0));
  nodes.push_back(MakeNode(1, "00000000-0000-0000-0000-0000000000F1", 1.0, 0.0, 0.0));
  nodes.push_back(MakeNode(2, "00000000-0000-0000-0000-0000000000F2", 1.0, 1.0, 0.0));
  nodes.push_back(MakeNode(3, "00000000-0000-0000-0000-0000000000F3", 0.0, 1.0, 0.0));
  nodes.push_back(MakeNode(4, "00000000-0000-0000-0000-0000000000F4", 0.12, 0.16, 0.0));
  return nodes;
}

TEST(LoopClosureToPoseGraph, MetricEligibleProducesOneEdge) {
  auto nodes = DriftedSquare();
  LoopClosureToPoseGraphInput in;
  in.trajectory_nodes = &nodes;
  in.closure = MakeAcceptedRevisit(nodes);
  in.metric_basis = MakeBasis();

  const LoopClosureToPoseGraphResult r = LoopClosureToPoseGraph(in);

  EXPECT_TRUE(r.metric_eligible);
  ASSERT_TRUE(r.metric_edge.has_value());
  EXPECT_EQ(r.metric_edge->type, "loop_closure");
  // INV-1: the real metric edge is NOT identity.
  EXPECT_TRUE(r.metric_edge->relative_position_xyz[0] != 0.0 ||
              r.metric_edge->relative_position_xyz[1] != 0.0 ||
              r.metric_edge->relative_position_xyz[2] != 0.0);
  EXPECT_TRUE(
      spatial::core::ValidateInformationMatrix(
          r.metric_edge->information_matrix_6x6).ok);
}

TEST(LoopClosureToPoseGraph, SpatialSeparationMIsDiagnosticAndDistinct) {
  auto nodes = DriftedSquare();
  LoopClosureToPoseGraphInput in;
  in.trajectory_nodes = &nodes;
  in.closure = MakeAcceptedRevisit(nodes);
  in.metric_basis = MakeBasis();

  const LoopClosureToPoseGraphResult r = LoopClosureToPoseGraph(in);

  // T5 / D2: spatial_separation_m == ||p4 - p0||_W = 0.2.
  EXPECT_NEAR(r.spatial_separation_m, 0.2, 1e-9);
  EXPECT_NEAR(r.closure.spatial_separation_m, 0.2, 1e-9);

  // It is a diagnostic distinct from the edge translation.
  ASSERT_TRUE(r.metric_edge.has_value());
  const auto& rel = r.metric_edge->relative_position_xyz;
  const double edge_mag = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] +
                                    rel[2] * rel[2]);
  EXPECT_GT(std::abs(r.spatial_separation_m - edge_mag), 1e-3);
}

TEST(LoopClosureToPoseGraph, UndeclaredBasisNoEdgeInv3) {
  auto nodes = DriftedSquare();
  LoopClosureToPoseGraphInput in;
  in.trajectory_nodes = &nodes;
  in.closure = MakeAcceptedRevisit(nodes);
  // metric_basis left default -> undeclared (INV-3).

  const LoopClosureToPoseGraphResult r = LoopClosureToPoseGraph(in);

  EXPECT_FALSE(r.metric_eligible);
  EXPECT_FALSE(r.metric_edge.has_value());
  // spatial_separation_m is still filled (diagnostic, independent of basis).
  EXPECT_NEAR(r.spatial_separation_m, 0.2, 1e-9);
}

TEST(LoopClosureToPoseGraph, ByFiatBasisNoEdgeInv3) {
  auto nodes = DriftedSquare();
  LoopClosureToPoseGraphInput in;
  in.trajectory_nodes = &nodes;
  in.closure = MakeAcceptedRevisit(nodes);
  MetricBasis bare;
  bare.declared = true;  // no scale ref / no provenance hash -> invalid
  in.metric_basis = bare;

  const LoopClosureToPoseGraphResult r = LoopClosureToPoseGraph(in);

  EXPECT_FALSE(r.metric_eligible);
  EXPECT_FALSE(r.metric_edge.has_value());
  // D4: ValidateMetricBasis rejects the by-fiat basis explicitly.
  EXPECT_FALSE(spatial::core::ValidateMetricBasis(bare).ok);
}

TEST(LoopClosureToPoseGraph, NoMeasurementNoEdgeInv1) {
  auto nodes = DriftedSquare();
  LoopClosureToPoseGraphInput in;
  in.trajectory_nodes = &nodes;
  LoopClosure lc = MakeAcceptedRevisit(nodes);
  lc.relative_position_xyz = {0.0, 0.0, 0.0};  // no metric measurement (INV-1)
  in.closure = lc;
  in.metric_basis = MakeBasis();

  const LoopClosureToPoseGraphResult r = LoopClosureToPoseGraph(in);

  EXPECT_TRUE(r.metric_eligible);            // basis is declared
  EXPECT_FALSE(r.metric_edge.has_value());   // but no measurement -> no edge
}

TEST(LoopClosureToPoseGraph, UnresolvedFramesSpatialZeroAndNoEdge) {
  auto nodes = DriftedSquare();
  LoopClosureToPoseGraphInput in;
  in.trajectory_nodes = &nodes;
  LoopClosure lc = MakeAcceptedRevisit(nodes);
  lc.target_frame_id = "00000000-0000-0000-0000-00000000ABAB";  // missing
  in.closure = lc;
  in.metric_basis = MakeBasis();

  const LoopClosureToPoseGraphResult r = LoopClosureToPoseGraph(in);

  EXPECT_EQ(r.spatial_separation_m, 0.0);
  EXPECT_FALSE(r.metric_edge.has_value());
}

}  // namespace
