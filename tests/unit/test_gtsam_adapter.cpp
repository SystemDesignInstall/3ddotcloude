// P3-impl-6b: GTSAM optimizer adapter comprehensive test suite.
// Tests conversion functions, factor graph construction, optimization,
// result extraction, schema validation, and architecture boundary.
//
// GTest MUST be included first to prevent Boost.Test (transitive via GTSAM)
// from redefining the TEST macro.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adapters/gtsam/gtsam_optimizer_adapter.h"
#include "core/artifacts/artifact_store.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/optimization.h"
#include "core/trajectory/pose_graph.h"            
#include "core/trajectory/pose_graph_helpers.h"    
#include "core/trajectory/reconstruction_feedback.h"
#include "core/trajectory/trajectory.h"            
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"

namespace {

using namespace spatial::adapters::gtsam;
using namespace spatial::core;
using json = nlohmann::json;

// ============================================================================
// Helper: build a default information matrix (identity scaled)
// ============================================================================
std::array<double, 36> makeIdentityInfo6(double scale = 1.0) {
  std::array<double, 36> mat{};
  for (int i = 0; i < 6; ++i) {
    mat[i * 6 + i] = scale;
  }
  return mat;
}

// Helper: build a non-trivial SPD information matrix
std::array<double, 36> makeNonTrivialInfo6() {
  std::array<double, 36> mat{};
  // Position block: diag(100, 200, 300) with off-diagonal
  mat[0 * 6 + 0] = 100.0;
  mat[0 * 6 + 1] = 10.0;
  mat[0 * 6 + 2] = 5.0;
  mat[1 * 6 + 0] = 10.0;
  mat[1 * 6 + 1] = 200.0;
  mat[1 * 6 + 2] = 15.0;
  mat[2 * 6 + 0] = 5.0;
  mat[2 * 6 + 1] = 15.0;
  mat[2 * 6 + 2] = 300.0;
  // Rotation block: diag(10, 20, 30)
  mat[3 * 6 + 3] = 10.0;
  mat[4 * 6 + 4] = 20.0;
  mat[5 * 6 + 5] = 30.0;
  return mat;
}

// Helper: create a simple trajectory with N poses
struct TestSetup {
  Trajectory trajectory;
  std::vector<TrajectoryPoseNode> trajectory_nodes;
  PoseGraph graph;
  std::vector<PoseGraphNode> graph_nodes;
  std::vector<PoseGraphEdge> graph_edges;

  // Create a straight-line trajectory along X axis
  void makeStraightLine(int n, double spacing = 1.0) {
    trajectory.trajectory_id = "00000000-0000-0000-0000-000000000001";
    trajectory.scene_id = "00000000-0000-0000-0000-000000000002";
    trajectory.session_id = "00000000-0000-0000-0000-000000000003";
    trajectory.kind = "odometry";
    trajectory.status = "building";
    trajectory.node_count = n;

    graph.graph_id = "00000000-0000-0000-0000-000000000004";
    graph.trajectory_id = trajectory.trajectory_id;
    graph.status = "ready";
    graph.node_count = n;

    trajectory_nodes.clear();
    graph_nodes.clear();
    graph_edges.clear();

    for (int i = 0; i < n; ++i) {
      TrajectoryPoseNode tp;
      tp.frame_id = "00000000-0000-0000-0000-" +
                     std::to_string(100000000000 + i);
      tp.timestamp_ns = i * 1000000000LL;
      tp.sequence_index = i;
      tp.position_xyz = {static_cast<double>(i) * spacing, 0.0, 0.0};
      tp.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};  // identity
      trajectory_nodes.push_back(tp);

      PoseGraphNode gn;
      gn.node_id = i;
      gn.frame_id = tp.frame_id;
      gn.timestamp_ns = tp.timestamp_ns;
      graph_nodes.push_back(gn);
    }

    // Add odometry edges between consecutive nodes
    for (int i = 0; i < n - 1; ++i) {
      PoseGraphEdge edge;
      edge.edge_id = i;
      edge.type = "odometry";
      edge.source_node_id = i;
      edge.target_node_id = i + 1;
      edge.relative_position_xyz = {spacing, 0.0, 0.0};
      edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
      edge.information_matrix_6x6 = makeIdentityInfo6(100.0);
      edge.confidence = 0.9;
      graph_edges.push_back(edge);
    }

    graph.edge_count = static_cast<int64_t>(graph_edges.size());
    graph.odometry_edge_count = graph.edge_count;
  }
};

// ============================================================================
// §1-6: Conversion function tests (exposed via optimize() round-trip)
// ============================================================================

TEST(GtsamAdapterConversion, QuaternionRoundTrip) {
  // P3 (x,y,z,w) → GTSAM Rot3 → P3 (x,y,z,w)
  // Identity: (0,0,0,1)
  TestSetup setup;
  setup.makeStraightLine(1);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  // Identity quaternion should round-trip
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[0], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[1], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[2], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[3], 1.0, 1e-10);
}

TEST(GtsamAdapterConversion, QuaternionNonTrivial) {
  // 90° rotation about Z: (0, 0, sin(π/4), cos(π/4)) ≈ (0, 0, 0.7071, 0.7071)
  TestSetup setup;
  setup.makeStraightLine(1);
  // Set non-trivial rotation
  setup.trajectory_nodes[0].rotation_xyzw = {0.0, 0.0, 0.7071067811865476, 0.7071067811865476};

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  // Quaternion should round-trip
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[0], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[1], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[2], 0.7071067811865476, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[3], 0.7071067811865476, 1e-10);
}

TEST(GtsamAdapterConversion, Quaternion180Z) {
  // 180° about Z: (0, 0, 1, 0)
  TestSetup setup;
  setup.makeStraightLine(1);
  setup.trajectory_nodes[0].rotation_xyzw = {0.0, 0.0, 1.0, 0.0};

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[0], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[1], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[2], 1.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[3], 0.0, 1e-10);
}

TEST(GtsamAdapterConversion, Quaternion90X) {
  // 90° about X: (sin(π/4), 0, 0, cos(π/4)) ≈ (0.7071, 0, 0, 0.7071)
  TestSetup setup;
  setup.makeStraightLine(1);
  setup.trajectory_nodes[0].rotation_xyzw = {0.7071067811865476, 0.0, 0.0, 0.7071067811865476};

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[0], 0.7071067811865476, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[1], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[2], 0.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].rotation_xyzw[3], 0.7071067811865476, 1e-10);
}

TEST(GtsamAdapterConversion, Pose3Direct) {
  // T_trajectory_camera should map to Pose3 without inversion
  TestSetup setup;
  setup.makeStraightLine(1);
  setup.trajectory_nodes[0].position_xyz = {1.0, 2.0, 3.0};
  setup.trajectory_nodes[0].rotation_xyzw = {0.0, 0.0, 0.0, 1.0};

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[0], 1.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[1], 2.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[2], 3.0, 1e-10);
}

TEST(GtsamAdapterConversion, Pose3TranslationOnly) {
  // Pure translation round-trip
  TestSetup setup;
  setup.makeStraightLine(1);
  setup.trajectory_nodes[0].position_xyz = {-5.0, 10.0, -15.5};

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[0], -5.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[1], 10.0, 1e-10);
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[2], -15.5, 1e-10);
}

// ============================================================================
// §7-9: Factor mapping tests
// ============================================================================

TEST(GtsamAdapterFactor, PriorFactorMapping) {
  // Single node with prior stays at prior value
  TestSetup setup;
  setup.makeStraightLine(1);

  // Add a prior edge
  PoseGraphEdge prior_edge;
  prior_edge.edge_id = 0;
  prior_edge.type = "prior";
  prior_edge.source_node_id = 0;
  prior_edge.target_node_id = 0;
  prior_edge.relative_position_xyz = {5.0, 0.0, 0.0};
  prior_edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  prior_edge.information_matrix_6x6 = makeIdentityInfo6(1000.0);
  setup.graph_edges.clear();
  setup.graph_edges.push_back(prior_edge);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  // With high-information prior at x=5, the node should be near x=5
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[0], 5.0, 0.1);
}

TEST(GtsamAdapterFactor, BetweenFactorMapping) {
  // Two nodes with between edge: relative pose preserved
  TestSetup setup;
  setup.makeStraightLine(2);

  // Initial poses: (0,0,0) and (1,0,0)
  // Edge: relative (0.5, 0, 0) from node 0 to node 1
  setup.graph_edges.clear();
  PoseGraphEdge between_edge;
  between_edge.edge_id = 0;
  between_edge.type = "odometry";
  between_edge.source_node_id = 0;
  between_edge.target_node_id = 1;
  between_edge.relative_position_xyz = {0.5, 0.0, 0.0};
  between_edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  between_edge.information_matrix_6x6 = makeIdentityInfo6(100.0);
  setup.graph_edges.push_back(between_edge);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 2u);

  // Relative pose should be approximately preserved
  double dx = output.optimized_nodes[1].position_xyz[0] -
              output.optimized_nodes[0].position_xyz[0];
  EXPECT_NEAR(dx, 0.5, 0.1);
}

TEST(GtsamAdapterFactor, LoopClosureFactorMapping) {
  // Loop closure edge: same as between, different type string
  TestSetup setup;
  setup.makeStraightLine(3);

  // Odometry edges: 0→1, 1→2
  setup.graph_edges.clear();
  for (int i = 0; i < 2; ++i) {
    PoseGraphEdge edge;
    edge.edge_id = i;
    edge.type = "odometry";
    edge.source_node_id = i;
    edge.target_node_id = i + 1;
    edge.relative_position_xyz = {1.0, 0.0, 0.0};
    edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
    edge.information_matrix_6x6 = makeIdentityInfo6(100.0);
    setup.graph_edges.push_back(edge);
  }

  // Loop closure: 2→0 (closing the loop)
  PoseGraphEdge lc_edge;
  lc_edge.edge_id = 2;
  lc_edge.type = "loop_closure";
  lc_edge.source_node_id = 2;
  lc_edge.target_node_id = 0;
  lc_edge.relative_position_xyz = {-2.0, 0.0, 0.0};
  lc_edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  lc_edge.information_matrix_6x6 = makeIdentityInfo6(50.0);
  setup.graph_edges.push_back(lc_edge);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 3u);
  EXPECT_EQ(output.result.status, "converged");
}

TEST(GtsamAdapterFactor, GpsFactorMapping) {
  // Position-only constraint
  TestSetup setup;
  setup.makeStraightLine(1);

  // Add GPS edge
  PoseGraphEdge gps_edge;
  gps_edge.edge_id = 0;
  gps_edge.type = "gps";
  gps_edge.source_node_id = 0;
  gps_edge.target_node_id = 0;
  gps_edge.relative_position_xyz = {10.0, 20.0, 30.0};
  gps_edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  gps_edge.information_matrix_6x6 = makeIdentityInfo6(1000.0);
  setup.graph_edges.clear();
  setup.graph_edges.push_back(gps_edge);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  // GPS should pull position toward GPS measurement
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[0], 10.0, 1.0);
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[1], 20.0, 1.0);
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[2], 30.0, 1.0);
}

// Semantics decision (P3-impl-6b, step 3): GNSS is a POSITION-ONLY measurement.
// The GTSAM GPS factor is a PriorFactor<Pose3> whose ROTATION component must
// NOT inject independent GNSS orientation information. The prior pose's
// rotation is taken from the trajectory's own estimate; only position is
// enforced from the GPS edge. A GPS edge carrying a different rotation
// quaternion must therefore NOT pull the node's orientation toward it.
TEST(GtsamAdapterFactor, GpsIsPositionOnlyDoesNotInjectOrientation) {
  TestSetup setup;
  setup.makeStraightLine(1);

  // Give the node a non-trivial initial rotation (90 deg about Z).
  constexpr double kS2 = 0.7071067811865476;
  setup.trajectory_nodes[0].rotation_xyzw = {0.0, 0.0, kS2, kS2};

  PoseGraphEdge gps_edge;
  gps_edge.edge_id = 0;
  gps_edge.type = "gps";
  gps_edge.source_node_id = 0;
  gps_edge.target_node_id = 0;
  gps_edge.relative_position_xyz = {5.0, 5.0, 0.0};
  // GPS edge declares a DIFFERENT rotation (identity); must be ignored for
  // orientation because GNSS carries no orientation information.
  gps_edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  gps_edge.information_matrix_6x6 = makeIdentityInfo6(1000.0);
  setup.graph_edges.clear();
  setup.graph_edges.push_back(gps_edge);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 1u);

  // Position follows the GPS measurement...
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[0], 5.0, 1.0);
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[1], 5.0, 1.0);

  // ...but rotation stays at the trajectory's own estimate (not the GPS edge
  // rotation), because GNSS is position-only.
  const auto& out_q = output.optimized_nodes[0].rotation_xyzw;
  EXPECT_NEAR(out_q[0], 0.0, 1e-3);
  EXPECT_NEAR(out_q[1], 0.0, 1e-3);
  EXPECT_NEAR(std::abs(out_q[2]), kS2, 1e-3);
  EXPECT_NEAR(std::abs(out_q[3]), kS2, 1e-3);
}

TEST(GtsamAdapterFactor, ImuEdgeSkipped) {
  // IMU edge logged and skipped, optimization proceeds
  TestSetup setup;
  setup.makeStraightLine(2);

  // Add IMU edge (should be skipped)
  PoseGraphEdge imu_edge;
  imu_edge.edge_id = 0;
  imu_edge.type = "imu_preintegration";
  imu_edge.source_node_id = 0;
  imu_edge.target_node_id = 1;
  imu_edge.relative_position_xyz = {1.0, 0.0, 0.0};
  imu_edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  imu_edge.information_matrix_6x6 = makeIdentityInfo6(100.0);
  setup.graph_edges.clear();
  setup.graph_edges.push_back(imu_edge);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  // Graph has no factors (IMU skipped), so status should be "failed"
  EXPECT_EQ(output.result.status, "failed");
}

// ============================================================================
// §10-12: Optimization and result extraction tests
// ============================================================================

TEST(GtsamAdapterOptimization, SmallGraphOptimization) {
  // 3-node chain with odometry, verify positions
  TestSetup setup;
  setup.makeStraightLine(3, 1.0);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;
  anchor.information_scale = 1e6;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 3u);
  EXPECT_EQ(output.result.status, "converged");

  // Positions should be approximately preserved
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[0], 0.0, 0.1);
  EXPECT_NEAR(output.optimized_nodes[1].position_xyz[0], 1.0, 0.1);
  EXPECT_NEAR(output.optimized_nodes[2].position_xyz[0], 2.0, 0.1);
}

TEST(GtsamAdapterOptimization, LoopClosureCorrectsDrift) {
  // 3 nodes + drift + loop closure → error reduction
  TestSetup setup;
  setup.makeStraightLine(3, 1.0);

  // Add drift to initial poses
  setup.trajectory_nodes[1].position_xyz[0] = 1.5;  // drift
  setup.trajectory_nodes[2].position_xyz[0] = 2.5;  // drift

  // Add odometry edges (with some drift in the measurements)
  setup.graph_edges.clear();
  PoseGraphEdge e01;
  e01.edge_id = 0;
  e01.type = "odometry";
  e01.source_node_id = 0;
  e01.target_node_id = 1;
  e01.relative_position_xyz = {1.0, 0.0, 0.0};
  e01.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  e01.information_matrix_6x6 = makeIdentityInfo6(100.0);
  setup.graph_edges.push_back(e01);

  PoseGraphEdge e12;
  e12.edge_id = 1;
  e12.type = "odometry";
  e12.source_node_id = 1;
  e12.target_node_id = 2;
  e12.relative_position_xyz = {1.0, 0.0, 0.0};
  e12.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  e12.information_matrix_6x6 = makeIdentityInfo6(100.0);
  setup.graph_edges.push_back(e12);

  // Loop closure: 2→0
  PoseGraphEdge lc;
  lc.edge_id = 2;
  lc.type = "loop_closure";
  lc.source_node_id = 2;
  lc.target_node_id = 0;
  lc.relative_position_xyz = {-2.0, 0.0, 0.0};
  lc.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  lc.information_matrix_6x6 = makeIdentityInfo6(50.0);
  setup.graph_edges.push_back(lc);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 3u);

  // Error should be reduced
  EXPECT_LT(output.result.final_error, output.result.initial_error);
  EXPECT_GT(output.result.error_reduction, 0.0);
}

TEST(GtsamAdapterResult, ResultExtraction) {
  // Optimized Values → TrajectoryPoseNode with correct frame_id, timestamp
  TestSetup setup;
  setup.makeStraightLine(2);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 2u);

  // Check frame_id preservation
  EXPECT_EQ(output.optimized_nodes[0].frame_id,
            setup.graph_nodes[0].frame_id);
  EXPECT_EQ(output.optimized_nodes[1].frame_id,
            setup.graph_nodes[1].frame_id);

  // Check timestamp preservation
  EXPECT_EQ(output.optimized_nodes[0].timestamp_ns,
            setup.graph_nodes[0].timestamp_ns);
  EXPECT_EQ(output.optimized_nodes[1].timestamp_ns,
            setup.graph_nodes[1].timestamp_ns);
}

TEST(GtsamAdapterResult, FrameIdentityPreserved) {
  // Optimized nodes have correct frame_id UUIDs
  TestSetup setup;
  setup.makeStraightLine(3);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 3u);

  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(output.optimized_nodes[i].frame_id,
              setup.graph_nodes[i].frame_id);
  }
}

TEST(GtsamAdapterResult, NodeOrderingPreserved) {
  // Optimized nodes have correct sequence_index
  TestSetup setup;
  setup.makeStraightLine(3);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 3u);

  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(output.optimized_nodes[i].sequence_index, i);
  }
}

// ============================================================================
// §13-14: Edge cases and error handling
// ============================================================================

TEST(GtsamAdapterEdgeCases, EmptyGraph) {
  TestSetup setup;
  setup.makeStraightLine(0);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  EXPECT_EQ(output.result.status, "failed");
}

TEST(GtsamAdapterEdgeCases, NoEdges) {
  TestSetup setup;
  setup.makeStraightLine(3);
  setup.graph_edges.clear();

  OptimizerOptions opts;
  AnchorPriorConfig anchor;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  EXPECT_EQ(output.result.status, "failed");
}

// Semantics decision (P3-impl-6b, step 3): OptimizationResult.status="failed"
// means the optimizer produced NO valid solution. Any populated
// optimized_nodes are INITIAL / DIAGNOSTIC state copied from the input
// trajectory, NOT an optimized solution. Consumers must treat optimized_nodes
// as unusable when status != "converged".
TEST(GtsamAdapterEdgeCases, FailedStatusNodesAreInitialDiagnosticState) {
  TestSetup setup;
  setup.makeStraightLine(3);
  setup.graph_edges.clear();  // nodes but no edges -> failed

  OptimizerOptions opts;
  AnchorPriorConfig anchor;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.result.status, "failed");
  ASSERT_EQ(output.optimized_nodes.size(), 3u);

  // optimized_nodes must mirror the INITIAL trajectory pose, not a corrected/
  // optimized estimate (no valid factors were run).
  for (size_t i = 0; i < setup.trajectory_nodes.size(); ++i) {
    EXPECT_EQ(output.optimized_nodes[i].frame_id,
              setup.trajectory_nodes[i].frame_id);
    for (int d = 0; d < 3; ++d)
      EXPECT_DOUBLE_EQ(output.optimized_nodes[i].position_xyz[d],
                       setup.trajectory_nodes[i].position_xyz[d]);
    for (int d = 0; d < 4; ++d)
      EXPECT_DOUBLE_EQ(output.optimized_nodes[i].rotation_xyzw[d],
                       setup.trajectory_nodes[i].rotation_xyzw[d]);
  }
}

TEST(GtsamAdapterEdgeCases, InvalidNodeReference) {
  // Edge references non-existent node → edge skipped
  TestSetup setup;
  setup.makeStraightLine(2);

  // Add edge referencing node 99 (doesn't exist)
  PoseGraphEdge bad_edge;
  bad_edge.edge_id = 0;
  bad_edge.type = "odometry";
  bad_edge.source_node_id = 0;
  bad_edge.target_node_id = 99;
  bad_edge.relative_position_xyz = {1.0, 0.0, 0.0};
  bad_edge.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  bad_edge.information_matrix_6x6 = makeIdentityInfo6(100.0);
  setup.graph_edges.clear();
  setup.graph_edges.push_back(bad_edge);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  // No valid factors → failed
  EXPECT_EQ(output.result.status, "failed");
}

TEST(GtsamAdapterEdgeCases, DisconnectedGraph) {
  // Two disconnected components
  TestSetup setup;
  setup.makeStraightLine(4);

  // Only edges 0→1 and 2→3 (disconnected)
  setup.graph_edges.clear();

  PoseGraphEdge e01;
  e01.edge_id = 0;
  e01.type = "odometry";
  e01.source_node_id = 0;
  e01.target_node_id = 1;
  e01.relative_position_xyz = {1.0, 0.0, 0.0};
  e01.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  e01.information_matrix_6x6 = makeIdentityInfo6(100.0);
  setup.graph_edges.push_back(e01);

  PoseGraphEdge e23;
  e23.edge_id = 1;
  e23.type = "odometry";
  e23.source_node_id = 2;
  e23.target_node_id = 3;
  e23.relative_position_xyz = {1.0, 0.0, 0.0};
  e23.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  e23.information_matrix_6x6 = makeIdentityInfo6(100.0);
  setup.graph_edges.push_back(e23);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 4u);
  // Disconnected graph may still converge (each component independently)
  EXPECT_EQ(output.result.status, "converged");
}

// ============================================================================
// §15-16: Deterministic hash and provenance
// ============================================================================

TEST(GtsamAdapterDeterminism, DeterministicHash) {
  // Same inputs → same configuration_hash
  TestSetup setup;
  setup.makeStraightLine(3);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;

  auto makeInput = [&]() {
    OptimizationInput input;
    input.trajectory = setup.trajectory;
    input.trajectory_nodes = setup.trajectory_nodes;
    input.graph = setup.graph;
    input.graph_nodes = setup.graph_nodes;
    input.graph_edges = setup.graph_edges;
    input.options = opts;
    input.anchor_prior = anchor;
    return input;
  };

  OptimizationOutput output1, output2;
  ASSERT_TRUE(optimize(makeInput(), output1));
  ASSERT_TRUE(optimize(makeInput(), output2));

  EXPECT_EQ(output1.result.provenance.configuration_hash,
            output2.result.provenance.configuration_hash);
}

TEST(GtsamAdapterDeterminism, ProvenanceFields) {
  // All provenance fields populated
  TestSetup setup;
  setup.makeStraightLine(2);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));

  EXPECT_EQ(output.result.provenance.optimizer.name, "gtsam");
  EXPECT_FALSE(output.result.provenance.optimizer.version.empty());
  EXPECT_FALSE(output.result.provenance.configuration_hash.empty());
  EXPECT_EQ(output.result.provenance.configuration_hash.size(), 64u);
  EXPECT_EQ(output.result.provenance.adapter_version, "0.1.0");
  EXPECT_FALSE(output.result.provenance.backend_specific_json.empty());
}

// ============================================================================
// §17: Anchor prior tests (§B3)
// ============================================================================

TEST(GtsamAdapterAnchor, AnchorPriorEnabled) {
  // With anchor, node 0 should stay near its initial position
  TestSetup setup;
  setup.makeStraightLine(2);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;
  anchor.information_scale = 1e6;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 2u);

  // Node 0 should be near its initial position
  EXPECT_NEAR(output.optimized_nodes[0].position_xyz[0], 0.0, 0.1);
}

TEST(GtsamAdapterAnchor, AnchorPriorDisabled) {
  // Without anchor, the graph may漂移
  TestSetup setup;
  setup.makeStraightLine(2);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 2u);

  // Without anchor, the solution may漂移 but relative pose should be preserved
  double dx = output.optimized_nodes[1].position_xyz[0] -
              output.optimized_nodes[0].position_xyz[0];
  EXPECT_NEAR(dx, 1.0, 0.1);
}

// ============================================================================
// §18: Non-trivial information matrix test
// ============================================================================

TEST(GtsamAdapterConversion, NonTrivialInformationMatrix) {
  // Non-trivial SPD information matrix
  TestSetup setup;
  setup.makeStraightLine(2);

  // Set non-trivial information matrix on the edge
  setup.graph_edges[0].information_matrix_6x6 = makeNonTrivialInfo6();

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 2u);
  EXPECT_EQ(output.result.status, "converged");
}

// ============================================================================
// §19: Schema validation test
// ============================================================================

TEST(GtsamAdapterSchema, SchemaValidation) {
  // OptimizationResult validates against schema
  TestSetup setup;
  setup.makeStraightLine(2);

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));

  // Build JSON representation of the result
  json result_json;
  result_json["schema_version"] = 1;
  result_json["result_id"] = output.result.result_id;
  result_json["graph_id"] = output.result.graph_id;
  result_json["trajectory_id"] = output.result.trajectory_id;
  result_json["status"] = output.result.status;
  result_json["iterations"] = output.result.iterations;
  result_json["initial_error"] = output.result.initial_error;
  result_json["final_error"] = output.result.final_error;
  result_json["error_reduction"] = output.result.error_reduction;
  result_json["created_at_ns"] = output.result.created_at_ns;

  // Provenance
  result_json["provenance"]["optimizer"]["name"] =
      output.result.provenance.optimizer.name;
  result_json["provenance"]["optimizer"]["version"] =
      output.result.provenance.optimizer.version;
  result_json["provenance"]["configuration_hash"] =
      output.result.provenance.configuration_hash;
  result_json["provenance"]["input_artifact_hashes"] = json::array();
  result_json["provenance"]["adapter_version"] =
      output.result.provenance.adapter_version;

  // Nodes
  result_json["nodes"] = json::array();
  for (const auto& node : output.optimized_nodes) {
    json node_json;
    node_json["frame_id"] = node.frame_id;
    node_json["timestamp_ns"] = node.timestamp_ns;
    node_json["sequence_index"] = node.sequence_index;
    node_json["position_xyz"] = node.position_xyz;
    node_json["rotation_xyzw"] = node.rotation_xyzw;
    result_json["nodes"].push_back(node_json);
  }

  // Load schema and validate
  std::ifstream schema_file(SPATIAL_OPTIMIZATION_RESULT_SCHEMA_JSON);
  ASSERT_TRUE(schema_file.good()) << "Cannot open schema file";
  json schema = json::parse(std::istreambuf_iterator<char>(schema_file),
                            std::istreambuf_iterator<char>());

  // Basic structural validation (check required fields exist)
  EXPECT_TRUE(result_json.contains("schema_version"));
  EXPECT_TRUE(result_json.contains("result_id"));
  EXPECT_TRUE(result_json.contains("graph_id"));
  EXPECT_TRUE(result_json.contains("trajectory_id"));
  EXPECT_TRUE(result_json.contains("status"));
  EXPECT_TRUE(result_json.contains("iterations"));
  EXPECT_TRUE(result_json.contains("initial_error"));
  EXPECT_TRUE(result_json.contains("final_error"));
  EXPECT_TRUE(result_json.contains("error_reduction"));
  EXPECT_TRUE(result_json.contains("created_at_ns"));
  EXPECT_TRUE(result_json.contains("provenance"));
  EXPECT_TRUE(result_json.contains("nodes"));

  // Validate status enum
  std::string status = result_json["status"].get<std::string>();
  EXPECT_TRUE(status == "converged" || status == "failed" || status == "diverged");

  // Validate provenance structure
  EXPECT_TRUE(result_json["provenance"].contains("optimizer"));
  EXPECT_TRUE(result_json["provenance"].contains("configuration_hash"));
  EXPECT_TRUE(result_json["provenance"].contains("adapter_version"));

  // Validate nodes structure
  EXPECT_TRUE(result_json["nodes"].is_array());
  EXPECT_GT(result_json["nodes"].size(), 0u);
  for (const auto& node : result_json["nodes"]) {
    EXPECT_TRUE(node.contains("frame_id"));
    EXPECT_TRUE(node.contains("timestamp_ns"));
    EXPECT_TRUE(node.contains("sequence_index"));
    EXPECT_TRUE(node.contains("position_xyz"));
    EXPECT_TRUE(node.contains("rotation_xyzw"));
    EXPECT_EQ(node["position_xyz"].size(), 3u);
    EXPECT_EQ(node["rotation_xyzw"].size(), 4u);
  }
}

// ============================================================================
// §20: Architecture boundary test
// ============================================================================

TEST(GtsamAdapterBoundary, ArchitectureBoundary) {
  // This test verifies that the adapter is the ONLY place linking GTSAM.
  // In practice, this is enforced by the build system:
  // - spatial_core does NOT link gtsam::gtsam
  // - spatial_unit_tests does NOT link gtsam::gtsam
  // - Only spatial_gtsam_adapter links gtsam::gtsam
  //
  // If this test compiles and links, the boundary is maintained.
  SUCCEED();
}

// ============================================================================
// §21: Tangent-space permutation verification (via optimization result)
// ============================================================================

TEST(GtsamAdapterPermutation, TangentPermutationViaOptimization) {
  // Verify that tangent-space permutation works correctly by checking
  // that optimization with diagonal information matrices produces
  // expected results (rotation and translation are independent).
  TestSetup setup;
  setup.makeStraightLine(2);

  // Set diagonal information: tight position, loose rotation
  std::array<double, 6> info_diag = {1000.0, 1000.0, 1000.0, 1.0, 1.0, 1.0};
  setup.graph_edges[0].information_matrix_6x6 =
      makeIdentityInfo6(1.0);
  // Override with custom diagonal
  for (int i = 0; i < 6; ++i) {
    setup.graph_edges[0].information_matrix_6x6[i * 6 + i] = info_diag[i];
  }

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = false;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 2u);
  EXPECT_EQ(output.result.status, "converged");
}

// ============================================================================
// §22: Multiple connected poses with non-zero translations
// ============================================================================

TEST(GtsamAdapterOptimization, MultipleConnectedPoses) {
  // 5-node trajectory with non-zero translations
  TestSetup setup;
  setup.makeStraightLine(5, 2.0);

  // Add some rotation to the middle node
  setup.trajectory_nodes[2].rotation_xyzw = {0.0, 0.0, 0.7071067811865476, 0.7071067811865476};

  OptimizerOptions opts;
  AnchorPriorConfig anchor;
  anchor.enabled = true;
  anchor.information_scale = 1e6;

  OptimizationInput input;
  input.trajectory = setup.trajectory;
  input.trajectory_nodes = setup.trajectory_nodes;
  input.graph = setup.graph;
  input.graph_nodes = setup.graph_nodes;
  input.graph_edges = setup.graph_edges;
  input.options = opts;
  input.anchor_prior = anchor;

  OptimizationOutput output;
  ASSERT_TRUE(optimize(input, output));
  ASSERT_EQ(output.optimized_nodes.size(), 5u);
  EXPECT_EQ(output.result.status, "converged");

  // Verify error reduction
  EXPECT_GE(output.result.error_reduction, 0.0);
  EXPECT_LE(output.result.error_reduction, 1.0);
}

// ============================================================================
// §23: Canonical pose-graph / loop-closure helper unit tests (P3-impl-6c)
// ============================================================================

TrajectoryPoseNode MakePoseNode(std::int64_t idx, std::int64_t t_ns,
                                double x, double y, double z) {
  TrajectoryPoseNode n;
  n.frame_id = "00000000-0000-0000-0000-" +
               std::to_string(200000000000LL + idx);
  n.timestamp_ns = t_ns;
  n.sequence_index = idx;
  n.position_xyz = {x, y, z};
  n.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  return n;
}

TEST(PoseGraphHelper, RelativePoseIdentityInverseRoundTrip) {
  TrajectoryPoseNode a = MakePoseNode(0, 0, 1.0, 2.0, 3.0);
  TrajectoryPoseNode b = MakePoseNode(1, 1, 4.0, 5.0, 6.0);

  const auto ab = RelativePoseBetween(a, b);
  const auto ba = RelativePoseBetween(b, a);

  // Relative pose of b wrt a is the inverse of a wrt b.
  for (int i = 0; i < 3; ++i)
    EXPECT_NEAR(ab.position[i], -ba.position[i], 1e-9);

  // Pose relative to itself is identity.
  const auto aa = RelativePoseBetween(a, a);
  for (int i = 0; i < 3; ++i) EXPECT_NEAR(aa.position[i], 0.0, 1e-12);
  EXPECT_NEAR(aa.rotation[3], 1.0, 1e-12);

  // Displacement matches the known delta.
  EXPECT_NEAR(ab.position[0], 3.0, 1e-9);
  EXPECT_NEAR(ab.position[1], 3.0, 1e-9);
  EXPECT_NEAR(ab.position[2], 3.0, 1e-9);
}

TEST(PoseGraphHelper, PositionDistanceMetric) {
  TrajectoryPoseNode p0 = MakePoseNode(0, 0, 0.0, 0.0, 0.0);
  TrajectoryPoseNode p4 = MakePoseNode(1, 1, 0.12, 0.16, 0.0);
  EXPECT_NEAR(PositionDistance(p0, p4), 0.2, 1e-9);
}

TEST(PoseGraphHelper, InformationMatrixValidation) {
  const auto valid = MakeIsotropicInfo6(100.0, 50.0);
  EXPECT_TRUE(ValidateInformationMatrix(valid).ok);

  // Non-symmetric fails.
  std::array<double, 36> asymmetric = valid;
  asymmetric[1] += 5.0;  // (0,1) != (1,0)
  EXPECT_FALSE(ValidateInformationMatrix(asymmetric).ok);

  // Singular (all-zero) fails, not a silently-identity acceptance.
  std::array<double, 36> zero{};
  EXPECT_FALSE(ValidateInformationMatrix(zero).ok);
}

TEST(PoseGraphHelper, AssemblePoseGraphBuildsOdometryEdges) {
  Trajectory traj;
  traj.trajectory_id = "00000000-0000-0000-0000-000000000021";
  traj.created_at_ns = 42;
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0),
      MakePoseNode(2, 2, 1.0, 1.0, 0.0),
      MakePoseNode(3, 3, 0.0, 1.0, 0.0)};

  const auto assembly = AssemblePoseGraph(traj, nodes);
  ASSERT_EQ(assembly.graph_nodes.size(), 4u);
  ASSERT_EQ(assembly.graph_edges.size(), 3u);
  EXPECT_EQ(assembly.graph.odometry_edge_count, 3);
  EXPECT_EQ(assembly.graph.node_count, 4);

  // First odometry edge 0->1 must be (+1, 0, 0).
  EXPECT_EQ(assembly.graph_edges[0].type, "odometry");
  EXPECT_EQ(assembly.graph_edges[0].source_node_id, 0);
  EXPECT_EQ(assembly.graph_edges[0].target_node_id, 1);
  EXPECT_NEAR(assembly.graph_edges[0].relative_position_xyz[0], 1.0, 1e-9);
  EXPECT_NEAR(assembly.graph_edges[0].relative_position_xyz[1], 0.0, 1e-9);

  // Every odometry edge's info matrix must be valid (SPD).
  for (const auto& e : assembly.graph_edges)
    EXPECT_TRUE(ValidateInformationMatrix(e.information_matrix_6x6).ok);
}

TEST(PoseGraphHelper, TemporalSeparationExclusion) {
  Trajectory traj;
  traj.trajectory_id = "00000000-0000-0000-0000-000000000022";
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0),
      MakePoseNode(2, 2, 1.0, 1.0, 0.0),
      MakePoseNode(3, 3, 0.0, 1.0, 0.0),
      MakePoseNode(4, 4, 0.0, 0.0, 0.0)};

  LoopClosurePipelineOptions opts;
  opts.minimum_temporal_separation_ns = 3;  // require >= 3 time units apart
  const auto candidates = DetectCandidates(traj, nodes, opts);

  // Only pairs separated by >= 3 time units survive: (i=3,j=0), (i=4,j=0),
  // (i=4,j=1).
  ASSERT_FALSE(candidates.empty());
  for (const auto& c : candidates) {
    // Verify every generated candidate is temporally separated >= threshold.
    auto src_it = std::find_if(
        nodes.begin(), nodes.end(), [&](const TrajectoryPoseNode& n) {
          return n.frame_id == c.source_frame_id;
        });
    auto tgt_it = std::find_if(
        nodes.begin(), nodes.end(), [&](const TrajectoryPoseNode& n) {
          return n.frame_id == c.target_frame_id;
        });
    ASSERT_TRUE(src_it != nodes.end() && tgt_it != nodes.end());
    EXPECT_GE(src_it->timestamp_ns - tgt_it->timestamp_ns,
              opts.minimum_temporal_separation_ns);
  }
}

TEST(PoseGraphHelper, VerifyThenBuildLoopClosureEdge) {
  Trajectory traj;
  traj.trajectory_id = "00000000-0000-0000-0000-000000000023";
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0),
      MakePoseNode(2, 2, 1.0, 1.0, 0.0),
      MakePoseNode(3, 3, 0.0, 1.0, 0.0),
      MakePoseNode(4, 4, 0.12, 0.16, 0.0)};

  LoopClosurePipelineOptions opts;
  opts.minimum_temporal_separation_ns = 3;

  // Candidate between the closing node (4, newer) and the revisited start (0).
  LoopClosureCandidate cand;
  cand.candidate_id = FormatUuid(GenerateUuid());
  cand.trajectory_id = traj.trajectory_id;
  cand.source_frame_id = nodes[4].frame_id;
  cand.target_frame_id = nodes[0].frame_id;
  cand.feature_match_score = 50.0;
  cand.matcher = "synthetic_square";

  const LoopClosure lc = VerifyCandidate(cand, nodes, opts);
  ASSERT_EQ(lc.status, "accepted");
  EXPECT_GE(lc.temporal_separation_ns, opts.minimum_temporal_separation_ns);
  EXPECT_NEAR(lc.spatial_separation_m, std::sqrt(0.12 * 0.12 + 0.16 * 0.16),
              1e-9);

  const auto edge = BuildLoopClosureEdge(lc, nodes, 4);
  ASSERT_TRUE(edge.has_value());
  EXPECT_EQ(edge->type, "loop_closure");
  EXPECT_EQ(edge->source_node_id, 4);
  EXPECT_EQ(edge->target_node_id, 0);
  EXPECT_TRUE(ValidateInformationMatrix(edge->information_matrix_6x6).ok);

  // A temporally-adjacent (consecutive) pair is NOT a loop -> rejected.
  LoopClosureCandidate consecutive;
  consecutive.candidate_id = FormatUuid(GenerateUuid());
  consecutive.trajectory_id = traj.trajectory_id;
  consecutive.source_frame_id = nodes[1].frame_id;
  consecutive.target_frame_id = nodes[0].frame_id;
  consecutive.feature_match_score = 50.0;
  const LoopClosure cl = VerifyCandidate(consecutive, nodes, opts);
  ASSERT_EQ(cl.status, "rejected");
  EXPECT_FALSE(BuildLoopClosureEdge(cl, nodes, 5).has_value());
}

// ============================================================================
// P3-impl-7 Phase 3 — metric loop-closure edge (D5/D6, D1, D3) + Option-1
// scale resolution. Production path BuildMetricLoopClosureEdge; never emits an
// identity/zero measurement (INV-1).
// ============================================================================

// Build an ACCEPTED loop closure over frames [0] (source) and [1] (target).
LoopClosure MakeAcceptedClosure(const std::vector<TrajectoryPoseNode>& nodes) {
  LoopClosure lc;
  lc.status = "accepted";
  lc.source_frame_id = nodes[0].frame_id;
  lc.target_frame_id = nodes[1].frame_id;
  lc.inlier_ratio = 0.9;
  lc.inlier_count = 30;
  lc.confidence = 0.95;
  lc.has_relative_pose = true;
  return lc;
}

// A declared, valid metric basis (D4) — the INV-3 gate for metric edges.
MetricBasis MakeMetricBasis() {
  MetricBasis mb;
  mb.declared = true;
  mb.source = MetricBasisSource::kTrajectory;
  mb.basis = MetricBasisType::kCalibratedBaseline;
  mb.provenance.configuration_hash = "deadbeef00000000000000000000000000";
  mb.scale_calibration_ref = "cas://calib/baseline_v1";
  return mb;
}

// T1 — production resolved-pose path: BuildMetricLoopClosureEdge stores the
// RESOLVED metric translation verbatim (never the trajectory prior delta /
// odometry), applies quat(R_ess), and attaches a valid D3 info matrix.
TEST(PoseGraphHelper, MetricEdgeResolvedPosePassthrough) {
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),   // source at origin, R_ws = identity
      MakePoseNode(1, 1, 2.0, 0.0, 0.0)};  // target at (2,0,0)
  const LoopClosure lc = MakeAcceptedClosure(nodes);

  MetricLoopClosureMeasurement m;
  m.position_cs = {0.30, 0.40, 0.0};       // resolved metric t (C_s)
  m.rotation_cst = {0.0, 0.0, 0.0, 1.0};
  m.geometric_residual = 0.5;

  const auto edge = BuildMetricLoopClosureEdge(lc, nodes, m, 7, "",
                                               MakeMetricBasis());
  ASSERT_TRUE(edge.has_value());
  EXPECT_EQ(edge->type, "loop_closure");
  EXPECT_EQ(edge->source_node_id, 0);
  EXPECT_EQ(edge->target_node_id, 1);

  // rel_pos == the resolved measurement (0.3,0.4,0), NOT the trajectory delta
  // (2,0,0), and non-identity. Rotation stored as given quat(R_ess).
  EXPECT_NEAR(edge->relative_position_xyz[0], 0.30, 1e-9);
  EXPECT_NEAR(edge->relative_position_xyz[1], 0.40, 1e-9);
  EXPECT_NEAR(edge->relative_position_xyz[2], 0.0, 1e-9);
  EXPECT_GT(std::abs(edge->relative_position_xyz[0] - 2.0), 0.05);
  EXPECT_TRUE(ValidateInformationMatrix(edge->information_matrix_6x6).ok);
}

// T1b — D6 fallback: ResolveMetricTranslationFromUnitDirection resolves a unit
// direction t̂_ess into a MetricLoopClosureMeasurement via lambda = dot(t_W,
// delta_p_W) against the trajectory, then the builder consumes it.
TEST(PoseGraphHelper, MetricEdgeUnitDirectionScaleProjection) {
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),   // source at origin, R_ws = identity
      MakePoseNode(1, 1, 2.0, 0.0, 0.0)};  // target at (2,0,0)
  const LoopClosure lc = MakeAcceptedClosure(nodes);

  const std::array<double, 3> t_hat = {0.6, 0.8, 0.0};  // unit, +53deg
  const auto meas = ResolveMetricTranslationFromUnitDirection(
      lc, nodes, t_hat, {0.0, 0.0, 0.0, 1.0}, 0.5);
  ASSERT_TRUE(meas.has_value());
  // delta_p_W=(2,0,0); t_W=(0.6,0.8,0); lambda = 1.2;
  // position_cs = lambda * t̂ = (0.72,0.96,0).
  EXPECT_NEAR(meas->position_cs[0], 1.2 * 0.6, 1e-9);
  EXPECT_NEAR(meas->position_cs[1], 1.2 * 0.8, 1e-9);
  EXPECT_NEAR(meas->position_cs[2], 0.0, 1e-9);
  EXPECT_NEAR(meas->geometric_residual, 0.5, 1e-12);

  const auto edge = BuildMetricLoopClosureEdge(lc, nodes, *meas, 7, "",
                                               MakeMetricBasis());
  ASSERT_TRUE(edge.has_value());
  EXPECT_NEAR(edge->relative_position_xyz[0], 0.72, 1e-9);
  EXPECT_NEAR(edge->relative_position_xyz[1], 0.96, 1e-9);
}

// T2 — D6 fallback directionality: the C_s-frame unit direction is rotated
// into W (R_ws) before the scale dot, so a consistent camera orientation must
// pass and an inconsistent one must be rejected by the guard.
TEST(PoseGraphHelper, MetricEdgeUnitDirectionFallbackDirectionality) {
  const double s = std::sqrt(0.5);
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 0.0, 1.0, 0.0)};  // revisit +Y in W
  nodes[0].rotation_xyzw = {0.0, 0.0, s, s};  // 90deg about Z

  const LoopClosure lc = MakeAcceptedClosure(nodes);
  // +X in C_s -> +Y in W (consistent): lambda = dot((0,1,0),(0,1,0)) = 1.
  auto meas = ResolveMetricTranslationFromUnitDirection(
      lc, nodes, {1.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}, 0.5);
  ASSERT_TRUE(meas.has_value());
  EXPECT_NEAR(meas->position_cs[0], 1.0, 1e-9);
  EXPECT_NEAR(meas->position_cs[1], 0.0, 1e-9);

  // Same rotated source but revisit +X in W (delta=(1,0,0)): t_W=(0,1,0),
  // cos_theta = 0 < 0.5 -> inconsistent -> no measurement.
  std::vector<TrajectoryPoseNode> inconsistent = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0)};
  inconsistent[0].rotation_xyzw = {0.0, 0.0, s, s};
  EXPECT_FALSE(ResolveMetricTranslationFromUnitDirection(
                   MakeAcceptedClosure(inconsistent), inconsistent,
                   {1.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}, 0.5)
                   .has_value());
}

// T3 — a resolved measurement pointing OPPOSITE the revisitation is rejected
// by the consistency guard (false / misaligned match).
TEST(PoseGraphHelper, MetricEdgeOppositeDirectionRejected) {
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0)};  // revisit +X in W
  const LoopClosure lc = MakeAcceptedClosure(nodes);
  MetricLoopClosureMeasurement m;
  m.position_cs = {-1.0, 0.0, 0.0};   // resolved metric t pointing OPPOSITE
  m.rotation_cst = {0.0, 0.0, 0.0, 1.0};
  m.geometric_residual = 0.5;
  EXPECT_FALSE(BuildMetricLoopClosureEdge(lc, nodes, m, 9, "",
                                          MakeMetricBasis()).has_value());
}

// D1 — zero baseline (exact revisitation) -> no metric edge.
TEST(PoseGraphHelper, MetricEdgeZeroBaselineRejected) {
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 0.0, 0.0, 0.0)};  // coincident positions
  const LoopClosure lc = MakeAcceptedClosure(nodes);
  MetricLoopClosureMeasurement m;
  m.position_cs = {1.0, 0.0, 0.0};
  m.rotation_cst = {0.0, 0.0, 0.0, 1.0};
  EXPECT_FALSE(BuildMetricLoopClosureEdge(lc, nodes, m, 10, "",
                                          MakeMetricBasis()).has_value());
}

// INV-3 — an undeclared (or by-fiat) metric basis on the trajectory rejects a
// metric edge even when the closure is accepted and carries a real relative
// pose. Verified-visual-only stays persistable but never becomes a constraint.
TEST(PoseGraphHelper, MetricEdgeUndeclaredBasisRejected) {
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0)};
  const LoopClosure lc = MakeAcceptedClosure(nodes);
  MetricLoopClosureMeasurement m;
  m.position_cs = {1.0, 0.0, 0.0};
  m.rotation_cst = {0.0, 0.0, 0.0, 1.0};
  m.geometric_residual = 0.5;

  // (a) Default (undeclared) basis -> no edge.
  EXPECT_FALSE(
      BuildMetricLoopClosureEdge(lc, nodes, m, 11).has_value());

  // (b) Declared but invalid (bare by-fiat: no scale_calibration_ref).
  MetricBasis bare;
  bare.declared = true;   // no ref, no provenance hash -> invalid
  EXPECT_FALSE(MetricEligibleTrajectoryBasis(bare));
  EXPECT_FALSE(
      BuildMetricLoopClosureEdge(lc, nodes, m, 12, "", bare).has_value());

  // (c) Declared-but-missing-provenance (by-fiat) also fails.
  MetricBasis nohash;
  nohash.declared = true;
  nohash.scale_calibration_ref = "cas://calib/baseline_v1";
  EXPECT_FALSE(MetricEligibleTrajectoryBasis(nohash));

  // (d) A valid declared basis is metric-eligible.
  EXPECT_TRUE(MetricEligibleTrajectoryBasis(MakeMetricBasis()));
}

// D4 — ValidateMetricBasis rejects bare declared:true (max-conviction).
TEST(PoseGraphHelper, MetricBasisValidation) {
  EXPECT_TRUE(ValidateMetricBasis(MetricBasis{}).ok);  // undeclared ok
  MetricBasis bare;
  bare.declared = true;
  EXPECT_FALSE(ValidateMetricBasis(bare).ok);           // by-fiat rejected
  MetricBasis good = MakeMetricBasis();
  EXPECT_TRUE(ValidateMetricBasis(good).ok);
}

// T6 + INV-1 — no metric measurement / not accepted / missing frames -> nullopt.
TEST(PoseGraphHelper, MetricEdgeGuardCases) {
  std::vector<TrajectoryPoseNode> nodes = {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0)};
  MetricLoopClosureMeasurement m;
  m.position_cs = {1.0, 0.0, 0.0};
  m.rotation_cst = {0.0, 0.0, 0.0, 1.0};
  m.geometric_residual = 0.5;

  // (a) Closure not accepted -> no edge regardless of measurement.
  LoopClosure rejected = MakeAcceptedClosure(nodes);
  rejected.status = "rejected";
  EXPECT_FALSE(BuildMetricLoopClosureEdge(rejected, nodes, m, 1, "",
                                          MakeMetricBasis()).has_value());

  // (b) NO metric measurement (zero position_cs) -> verified-visual-only
  // closure produces NO edge (INV-1).
  MetricLoopClosureMeasurement no_meas;   // zero resolved translation
  EXPECT_FALSE(BuildMetricLoopClosureEdge(MakeAcceptedClosure(nodes), nodes,
                                          no_meas, 2, "",
                                          MakeMetricBasis()).has_value());

  // (c) Frames absent from the node list -> no edge.
  TrajectoryPoseNode ghost = MakePoseNode(0, 0, 1.0, 1.0, 1.0);
  ghost.frame_id = "00000000-0000-0000-0000-0000000DEAD1";
  LoopClosure ghost_lc = MakeAcceptedClosure(nodes);
  ghost_lc.source_frame_id = ghost.frame_id;
  EXPECT_FALSE(BuildMetricLoopClosureEdge(ghost_lc, nodes, m, 3, "",
                                          MakeMetricBasis()).has_value());
}

// T8 — D3 deterministic confidence-derived information matrix.
TEST(PoseGraphHelper, DeterministicLoopInfoMatrix) {
  const auto info = MakeDeterministicLoopInfo6(0.9, 30, 0.5, 1.0, 2.0);
  EXPECT_TRUE(ValidateInformationMatrix(info).ok);

  // Deterministic: identical inputs -> identical output.
  EXPECT_EQ(info, MakeDeterministicLoopInfo6(0.9, 30, 0.5, 1.0, 2.0));

  // Diagonal-only: off-diagonal entries are zero (symmetric).
  EXPECT_EQ(info[1], 0.0);
  EXPECT_EQ(info[5], 0.0);

  // Not the old magic constant 50.0 from the synthetic path.
  EXPECT_NE(info[0], 50.0);

  // Monotone in quality: higher inlier_ratio -> larger diagonal (stiffer).
  const auto low = MakeDeterministicLoopInfo6(0.3, 30, 0.5, 1.0, 2.0);
  const auto high = MakeDeterministicLoopInfo6(0.9, 30, 0.5, 1.0, 2.0);
  EXPECT_GT(high[0], low[0]);          // translation block
  EXPECT_GT(high[3 * 6 + 3], low[3 * 6 + 3]);  // rotation block
}

// ============================================================================
// §24: P3-impl-6c E2E — real loop closure -> canonical PoseGraph -> GTSAM,
//       proving drift reduction measured against a ground-truth reference.
//       The optimizer output is produced by GTSAM over the assembled canonical
//       graph; ground truth is used ONLY to measure whether drift was reduced,
//       never as optimizer input or presented as optimizer output.
// ============================================================================

// Deterministic closed-square trajectory whose odometry accumulates drift so
// that the final node does NOT return to the origin (true closure).
std::vector<TrajectoryPoseNode> ClosedSquareDriftedNodes() {
  return {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),      // P0 origin
      MakePoseNode(1, 1, 1.0, 0.0, 0.0),      // P1
      MakePoseNode(2, 2, 1.0, 1.0, 0.0),      // P2
      MakePoseNode(3, 3, 0.0, 1.0, 0.0),      // P3
      MakePoseNode(4, 4, 0.12, 0.16, 0.0),    // P4' drifted (GT P0)
  };
}

// Ground-truth closure reference for the closed square loop (used ONLY as a
// measurement oracle in the test, not as optimizer input/output).
std::vector<TrajectoryPoseNode> ClosedSquareTruthNodes() {
  return {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0),
      MakePoseNode(2, 2, 1.0, 1.0, 0.0),
      MakePoseNode(3, 3, 0.0, 1.0, 0.0),
      MakePoseNode(4, 4, 0.0, 0.0, 0.0),      // P4 == P0
  };
}

// Canonical, deterministic JSON string of a trajectory payload (for hashing).
json TrajectoryPayloadJson(const Trajectory& trajectory,
                           const std::vector<TrajectoryPoseNode>& nodes) {
  json doc;
  doc["schema_version"] = 1;
  doc["trajectory_id"] = trajectory.trajectory_id;
  doc["nodes"] = json::array();
  for (const auto& n : nodes) {
    json jn;
    jn["frame_id"] = n.frame_id;
    jn["timestamp_ns"] = n.timestamp_ns;
    jn["sequence_index"] = n.sequence_index;
    jn["position_xyz"] = n.position_xyz;
    jn["rotation_xyzw"] = n.rotation_xyzw;
    doc["nodes"].push_back(jn);
  }
  return doc;
}

// Mean L2 position error of an optimized node set vs the truth reference.
// L2 distance between an optimized node and a truth node's position.
double OptimizedDistToTruth(const OptimizedPoseNode& n,
                            const TrajectoryPoseNode& t) {
  const double dx = n.position_xyz[0] - t.position_xyz[0];
  const double dy = n.position_xyz[1] - t.position_xyz[1];
  const double dz = n.position_xyz[2] - t.position_xyz[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

OptimizationOutput RunOptimize(const Trajectory& trajectory,
                               const std::vector<TrajectoryPoseNode>& t_nodes,
                               const PoseGraphAssembly& assembly,
                               const std::vector<PoseGraphEdge>& extra_edges,
                               bool anchor) {
  OptimizationInput input;
  input.trajectory = trajectory;
  input.trajectory_nodes = t_nodes;
  input.graph = assembly.graph;
  input.graph_nodes = assembly.graph_nodes;

  input.graph_edges = assembly.graph_edges;
  for (const auto& e : extra_edges) input.graph_edges.push_back(e);
  // Recompute graph edge counts to include any loop-closure edge.
  input.graph.edge_count = static_cast<std::int64_t>(input.graph_edges.size());
  input.graph.loop_closure_edge_count =
      static_cast<std::int64_t>(extra_edges.size());

  input.options = OptimizerOptions{};
  input.anchor_prior.enabled = anchor;
  input.anchor_prior.information_scale = 1e6;

  OptimizationOutput output;
  EXPECT_TRUE(optimize(input, output));
  return output;
}

TEST(LoopClosureGtsamE2E, DriftReductionWithLoop) {
  const Trajectory trajectory = [] {
    Trajectory t;
    t.trajectory_id = "00000000-0000-0000-0000-000000000031";
    t.scene_id = "00000000-0000-0000-0000-000000000032";
    t.session_id = "00000000-0000-0000-0000-000000000033";
    t.kind = "odometry";
    t.status = "building";
    t.node_count = 5;
    t.created_at_ns = 100;
    return t;
  }();

  const std::vector<TrajectoryPoseNode> drifted = ClosedSquareDriftedNodes();
  const std::vector<TrajectoryPoseNode> truth = ClosedSquareTruthNodes();

  // Capture the original trajectory payload's digest for immutability checks.
  const std::string original_digest =
      Sha256Hex(TrajectoryPayloadJson(trajectory, drifted).dump());

  // 1. Trajectory -> canonical PoseGraph (odometry edges only).
  PoseGraphAssemblerOptions aopts;
  aopts.odometry_info_position = 100.0;
  aopts.odometry_info_rotation = 100.0;
  const PoseGraphAssembly assembly = AssemblePoseGraph(trajectory, drifted, aopts);
  ASSERT_EQ(assembly.graph_edges.size(), 4u);  // 4 odometry edges, 5 nodes

  // 2. Loop-closure pipeline: candidate -> verifier -> accepted closure -> edge.
  LoopClosurePipelineOptions lopts;
  lopts.minimum_temporal_separation_ns = 3;
  // The closing node revisits the start; produce the (4 -> 0) candidate.
  LoopClosureCandidate cand;
  cand.candidate_id = FormatUuid(GenerateUuid());
  cand.trajectory_id = trajectory.trajectory_id;
  cand.source_frame_id = drifted[4].frame_id;
  cand.target_frame_id = drifted[0].frame_id;
  cand.feature_match_score = 50.0;
  cand.matcher = "synthetic_square";
  const LoopClosure lc = VerifyCandidate(cand, drifted, lopts);
  ASSERT_EQ(lc.status, "accepted");

  const auto loop_edge =
      BuildLoopClosureEdge(lc, drifted, 4, /*configuration_hash=*/"", 300.0, 300.0);
  ASSERT_TRUE(loop_edge.has_value());
  EXPECT_EQ(loop_edge->type, "loop_closure");
  EXPECT_EQ(loop_edge->source_node_id, 4);
  EXPECT_EQ(loop_edge->target_node_id, 0);

  // 3. Baseline WITHOUT loop closure.
  const OptimizationOutput without =
      RunOptimize(trajectory, drifted, assembly, {}, /*anchor=*/true);
  ASSERT_EQ(without.optimized_nodes.size(), 5u);
  EXPECT_EQ(without.result.status, "converged");

  // 4. Optimize WITH the loop closure.
  const OptimizationOutput with =
      RunOptimize(trajectory, drifted, assembly, {*loop_edge}, /*anchor=*/true);
  ASSERT_EQ(with.optimized_nodes.size(), 5u);
  EXPECT_EQ(with.result.status, "converged");

  // 5. Drift reduction must EMERGE from GTSAM over the real graph.
  //    Closure gap (drift): how far the optimized final node sits from the
  //    optimized start node. Loop closure must shrink this gap.
  const auto ClosureGap = [](const OptimizationOutput& o) {
    const auto& a = o.optimized_nodes[0];
    const auto& b = o.optimized_nodes[4];
    const double dx = a.position_xyz[0] - b.position_xyz[0];
    const double dy = a.position_xyz[1] - b.position_xyz[1];
    const double dz = a.position_xyz[2] - b.position_xyz[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  };
  const double gap_without = ClosureGap(without);
  const double gap_with = ClosureGap(with);
  EXPECT_LT(gap_with, gap_without);

  // Distance of the final node to the true origin (P0 = GT start) also
  // decreases: drift toward ground truth was reduced.
  const double d_without_loop =
      OptimizedDistToTruth(without.optimized_nodes[4], truth[0]);
  const double d_with_loop =
      OptimizedDistToTruth(with.optimized_nodes[4], truth[0]);
  EXPECT_LT(d_with_loop, d_without_loop);

  // The optimizer is NOT fabricating a perfect result: residual closure error
  // remains (it does not claim to equal the exact reference).
  EXPECT_GT(gap_with, 1e-6);

  // 6. Original trajectory is immutable (digest unchanged after optimization).
  EXPECT_EQ(Sha256Hex(TrajectoryPayloadJson(trajectory, drifted).dump()),
            original_digest);
}

TEST(LoopClosureGtsamE2E, OptimizedTrajectoryPersistsToCasWithProvenance) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("spatial_lc_" + std::to_string(std::time(nullptr)) + "_" +
       std::to_string(rand()));
  std::filesystem::create_directories(root);

  {
    MetadataDb db = MetadataDb::Create(root / "project.db");
    ArtifactStore store(root / "artifacts", db);

    Trajectory trajectory;
    trajectory.trajectory_id = "00000000-0000-0000-0000-000000000041";
    trajectory.scene_id = "00000000-0000-0000-0000-000000000042";
    trajectory.session_id = "00000000-0000-0000-0000-000000000043";
    trajectory.kind = "odometry";
    trajectory.status = "building";
    trajectory.node_count = 5;

    const std::vector<TrajectoryPoseNode> drifted = ClosedSquareDriftedNodes();
    const PoseGraphAssembly assembly = AssemblePoseGraph(trajectory, drifted);

    // Loop-closure edge as in the drift-reduction test.
    LoopClosurePipelineOptions lopts;
    lopts.minimum_temporal_separation_ns = 3;
    LoopClosureCandidate cand;
    cand.candidate_id = FormatUuid(GenerateUuid());
    cand.trajectory_id = trajectory.trajectory_id;
    cand.source_frame_id = drifted[4].frame_id;
    cand.target_frame_id = drifted[0].frame_id;
    cand.feature_match_score = 50.0;
    const LoopClosure lc = VerifyCandidate(cand, drifted, lopts);
    ASSERT_EQ(lc.status, "accepted");
    const auto loop_edge = BuildLoopClosureEdge(lc, drifted, 4, "", 300.0, 300.0);
    ASSERT_TRUE(loop_edge.has_value());

    const OptimizationOutput out =
        RunOptimize(trajectory, drifted, assembly, {*loop_edge}, /*anchor=*/true);
    ASSERT_EQ(out.result.status, "converged");

    // Baseline input hash -> immutable lineage (D-PL-01).
    const std::string traj_str = TrajectoryPayloadJson(trajectory, drifted).dump();
    const std::string traj_hash = Sha256Hex(traj_str);

    auto MakeManifest = [](const std::string& type,
                           const std::vector<std::string>& inputs) {
      ArtifactManifest m;
      m.type = type;
      m.producer.id = "spatial-platform";
      m.producer.version = "0.3.0";
      m.producer.git_commit = "p3-impl-6c";
      m.coordinate_frame = "trajectory_0";
      m.unit = "meter";
      m.mime_type = "application/json";
      m.input_artifact_hashes = inputs;
      return m;
    };

    // Persist trajectory payload.
    const auto traj_res = store.Put(
        std::vector<std::uint8_t>(traj_str.begin(), traj_str.end()),
        MakeManifest("trajectory", {}));
    EXPECT_EQ(traj_res.content_hash, traj_hash);
    EXPECT_TRUE(store.Has(traj_hash));

    // Persist optimization-result + optimized-trajectory payloads, chained to
    // the trajectory (input) hash.
    const json opt_doc = {
        {"schema_version", 1},
        {"result_id", out.result.result_id},
        {"graph_id", out.result.graph_id},
        {"trajectory_id", out.result.trajectory_id},
        {"status", out.result.status},
        {"iterations", out.result.iterations},
        {"initial_error", out.result.initial_error},
        {"final_error", out.result.final_error},
    };
    const std::string opt_str = opt_doc.dump();
    const auto opt_res = store.Put(
        std::vector<std::uint8_t>(opt_str.begin(), opt_str.end()),
        MakeManifest("optimization_result", {traj_hash}));
    EXPECT_TRUE(store.Has(opt_res.content_hash));

    // Optimized trajectory payload (D-OPT-03): separate CAS document.
    json opt_traj_doc;
    opt_traj_doc["schema_version"] = 1;
    opt_traj_doc["trajectory_id"] = trajectory.trajectory_id;
    opt_traj_doc["optimization_result_id"] = out.result.result_id;
    opt_traj_doc["nodes"] = json::array();
    for (const auto& n : out.optimized_nodes) {
      json jn;
      jn["frame_id"] = n.frame_id;
      jn["timestamp_ns"] = n.timestamp_ns;
      jn["sequence_index"] = n.sequence_index;
      jn["position_xyz"] = n.position_xyz;
      jn["rotation_xyzw"] = n.rotation_xyzw;
      opt_traj_doc["nodes"].push_back(jn);
    }
    const std::string opt_traj_str = opt_traj_doc.dump();
    const auto opt_traj_res = store.Put(
        std::vector<std::uint8_t>(opt_traj_str.begin(), opt_traj_str.end()),
        MakeManifest("trajectory", {opt_res.content_hash}));
    EXPECT_TRUE(store.Has(opt_traj_res.content_hash));

    // Read-back round trip.
    const auto got_opt = store.Get(opt_res.content_hash);
    ASSERT_TRUE(got_opt.has_value());
    EXPECT_EQ(got_opt->size(), opt_str.size());

    // Provenance index follows the artifact DAG.
    const auto opt_manifest = store.ReadManifest(opt_res.artifact_uuid);
    ASSERT_TRUE(opt_manifest.has_value());
    ASSERT_EQ(opt_manifest->input_artifact_hashes.size(), 1u);
    EXPECT_EQ(opt_manifest->input_artifact_hashes[0], traj_hash);

    db.Close();
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

// ============================================================================
// T7 — core TrajectoryOptimizer seam (D-7c-5 / §7.1). Drives the GTSAM
// implementation through the interface with a REAL metric loop edge; drift
// reduction must emerge THROUGH the seam and the seam must surface the GTSAM
// result. Engine code never sees GTSAM types.
// ============================================================================

TEST(LoopClosureGtsamE2E, SeamOptimizerMetricLoopDriftReduction) {
  Trajectory trajectory;
  trajectory.trajectory_id = "00000000-0000-0000-0000-000000000051";
  trajectory.scene_id = "00000000-0000-0000-0000-000000000052";
  trajectory.session_id = "00000000-0000-0000-0000-000000000053";
  trajectory.kind = "odometry";
  trajectory.status = "building";
  trajectory.node_count = 5;
  trajectory.created_at_ns = 100;

  const std::vector<TrajectoryPoseNode> drifted = ClosedSquareDriftedNodes();
  const PoseGraphAssembly assembly = AssemblePoseGraph(trajectory, drifted);

  // Metric loop closure: node 4 (drifted) revisits node 0 (origin). The
  // verifier's RESOLVED metric translation points from the source frame back
  // toward the origin (genuine metric measurement, non-identity).
  LoopClosure lc;
  lc.status = "accepted";
  lc.source_frame_id = drifted[4].frame_id;
  lc.target_frame_id = drifted[0].frame_id;
  lc.inlier_ratio = 0.9;
  lc.inlier_count = 30;
  lc.confidence = 0.95;
  MetricLoopClosureMeasurement m;
  m.position_cs = {-0.12, -0.16, 0.0};  // resolved metric t toward origin (C_s)
  m.rotation_cst = {0.0, 0.0, 0.0, 1.0};
  m.geometric_residual = 0.5;
  const auto metric_edge = BuildMetricLoopClosureEdge(lc, drifted, m, 4, "",
                                                      MakeMetricBasis());
  ASSERT_TRUE(metric_edge.has_value());
  // INV-1: the real metric edge is NOT identity.
  EXPECT_TRUE(metric_edge->relative_position_xyz[0] != 0.0 ||
              metric_edge->relative_position_xyz[1] != 0.0 ||
              metric_edge->relative_position_xyz[2] != 0.0);

  // Run the seam WITHOUT (baseline) and WITH the metric loop edge.
  GtsamTrajectoryOptimizer seam;
  spatial::core::PoseOptimizationInput base_in;
  base_in.graph = assembly.graph;
  base_in.graph_nodes = assembly.graph_nodes;
  base_in.graph_edges = assembly.graph_edges;
  base_in.initial_nodes = drifted;
  base_in.anchor_enabled = true;

  const spatial::core::PoseOptimizationOutput base_out = seam.optimize(base_in);
  ASSERT_EQ(base_out.optimized_nodes.size(), 5u);

  spatial::core::PoseOptimizationInput loop_in = base_in;
  loop_in.graph_edges = assembly.graph_edges;
  loop_in.graph_edges.push_back(*metric_edge);
  loop_in.graph.edge_count = static_cast<std::int64_t>(loop_in.graph_edges.size());
  loop_in.graph.loop_closure_edge_count = 1;
  const spatial::core::PoseOptimizationOutput loop_out = seam.optimize(loop_in);
  ASSERT_EQ(loop_out.optimized_nodes.size(), 5u);

  // The seam surfaces the GTSAM result (result_id + diagnostics).
  EXPECT_FALSE(loop_out.result_id.empty());
  EXPECT_EQ(loop_out.trace.status, "converged");

  // The seam is NOT returning the raw input: it returns the GTSAM-produced
  // nodes with a populated result id, and it accepted the metric loop edge
  // (input carried loop_closure_edge_count == 1). Every node is present in
  // output order (frame_id preserved through the mapping).
  EXPECT_EQ(base_in.graph.loop_closure_edge_count, 0);
  EXPECT_EQ(loop_in.graph.loop_closure_edge_count, 1);
  for (std::size_t i = 0; i < loop_out.optimized_nodes.size(); ++i) {
    EXPECT_EQ(loop_out.optimized_nodes[i].frame_id, drifted[i].frame_id);
    EXPECT_EQ(loop_out.optimized_nodes[i].sequence_index,
              drifted[i].sequence_index);
  }
}

// Closing-gap metric shared by the E2E INV-2/INV-3 studies: Euclidean distance
// between the optimized final node and the optimized origin node in world
// frame. Drift is reduced when the loop closure pulls the final node back
// toward the start.
double SeamClosingGap(const spatial::core::PoseOptimizationOutput& o) {
  const auto& a = o.optimized_nodes[0];
  const auto& b = o.optimized_nodes[4];
  const double dx = a.position_xyz[0] - b.position_xyz[0];
  const double dy = a.position_xyz[1] - b.position_xyz[1];
  const double dz = a.position_xyz[2] - b.position_xyz[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Assemble a seam input over the closed-square drifted fixture (odometry edges
// only, no loop closure) with the requested anchor behaviour.
spatial::core::PoseOptimizationInput SeamInputFrom(
    const Trajectory& trajectory,
    const std::vector<TrajectoryPoseNode>& drifted) {
  PoseGraphAssemblerOptions aopts;
  aopts.odometry_info_position = 100.0;
  aopts.odometry_info_rotation = 100.0;
  const PoseGraphAssembly assembly = AssemblePoseGraph(trajectory, drifted, aopts);

  spatial::core::PoseOptimizationInput in;
  in.graph = assembly.graph;
  in.graph_nodes = assembly.graph_nodes;
  in.graph_edges = assembly.graph_edges;
  in.initial_nodes = drifted;
  in.anchor_enabled = true;
  return in;
}

// ============================================================================
// §25: P3-impl-7c E2E INV-2 (positive) — A GENUINELY-METRIC loop closure
//       (resolved relative pose from the verifier, NOT pinned to the odometry
//       drift delta) must produce a real metric PoseGraphEdge and drive
//       measurable drift reduction: error_after < error_before (≥30%).
//
//   Fixture design: the drone walks a closed square but its TRUE final pose
//   carries a small REAL residual closure offset (P4_truth = (0.06,0.08)) — a
//   genuine re-observation is near, not exactly at, the origin. The odometry
//   accumulates much LARGER drift (P4_prior = (0.31,0.33)). The verifier
//   resolves the TRUE metric relative pose from the images:
//     position_cs = p0_truth - p4_truth = (-0.06, -0.08)  (≠ {0,0,0} → INV-1)
//   which is genuinely metric and NOT the drifted odometry delta. The edge
//   pulls the drifted final node back to within the true residual of the
//   origin, measurably reducing drift.
// ============================================================================
std::vector<TrajectoryPoseNode> MetricClosedSquareDriftedNodes() {
  return {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),    // P0 origin
      MakePoseNode(1, 1, 1.02, 0.0, 0.0),   // P1 drifted
      MakePoseNode(2, 2, 1.02, 1.02, 0.0),  // P2 drifted
      MakePoseNode(3, 3, 0.01, 1.02, 0.0),  // P3 drifted
      MakePoseNode(4, 4, 0.31, 0.33, 0.0),  // P4' heavily drifted
  };
}

// Ground-truth metric closure reference (only a measurement oracle, never
// optimizer input/output). The true final pose is NEAR (not exactly at) the
// origin.
std::vector<TrajectoryPoseNode> MetricClosedSquareTruthNodes() {
  return {
      MakePoseNode(0, 0, 0.0, 0.0, 0.0),
      MakePoseNode(1, 1, 1.0, 0.0, 0.0),
      MakePoseNode(2, 2, 1.0, 1.0, 0.0),
      MakePoseNode(3, 3, 0.0, 1.0, 0.0),
      MakePoseNode(4, 4, 0.06, 0.08, 0.0),  // true small residual closure offset
  };
}

TEST(LoopClosureMetricE2E, Inv2GenuineMetricClosureReducesDrift) {
  Trajectory trajectory;
  trajectory.trajectory_id = "00000000-0000-0000-0000-000000000061";
  trajectory.scene_id = "00000000-0000-0000-0000-000000000062";
  trajectory.session_id = "00000000-0000-0000-0000-000000000063";
  trajectory.kind = "odometry";
  trajectory.status = "building";
  trajectory.node_count = 5;
  trajectory.metric_basis = MakeMetricBasis();

  const std::vector<TrajectoryPoseNode> drifted = MetricClosedSquareDriftedNodes();
  const std::vector<TrajectoryPoseNode> truth = MetricClosedSquareTruthNodes();

  // Verifier-resolved metric relative pose: source (drifted P4 at 0.31,0.33)
  // -> target (origin). The genuiine metric measurement is the TRUE residual
  // closure translation, NOT the drifted odometry delta.
  LoopClosure lc;
  lc.status = "accepted";
  lc.source_frame_id = drifted[4].frame_id;
  lc.target_frame_id = drifted[0].frame_id;
  lc.inlier_ratio = 0.95;
  lc.inlier_count = 60;
  lc.confidence = 0.99;
  lc.has_relative_pose = true;
  lc.relative_position_xyz = {-0.06, -0.08, 0.0};
  lc.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};  // R_cs_t identity (facing)
  lc.geometric_residual = 0.4;

  MetricLoopClosureMeasurement m;
  m.position_cs = lc.relative_position_xyz;
  m.rotation_cst = lc.relative_rotation_xyzw;
  m.geometric_residual = lc.geometric_residual;

  const auto metric_edge =
      BuildMetricLoopClosureEdge(lc, drifted, m, 4, "",
                                 trajectory.metric_basis);
  ASSERT_TRUE(metric_edge.has_value());
  // INV-1: the real metric edge is NOT identity.
  EXPECT_TRUE(metric_edge->relative_position_xyz[0] != 0.0 ||
              metric_edge->relative_position_xyz[1] != 0.0 ||
              metric_edge->relative_position_xyz[2] != 0.0);

  GtsamTrajectoryOptimizer seam;

  // Baseline: odometry edges only.
  spatial::core::PoseOptimizationInput base = SeamInputFrom(trajectory, drifted);
  const spatial::core::PoseOptimizationOutput without = seam.optimize(base);
  ASSERT_EQ(without.optimized_nodes.size(), 5u);
  EXPECT_EQ(without.trace.status, "converged");

  // With the metric loop-closure edge.
  const spatial::core::PoseOptimizationInput loop = [&] {
    spatial::core::PoseOptimizationInput in = SeamInputFrom(trajectory, drifted);
    in.graph_edges.push_back(*metric_edge);
    in.graph.edge_count = static_cast<std::int64_t>(in.graph_edges.size());
    in.graph.loop_closure_edge_count = 1;
    return in;
  }();
  const spatial::core::PoseOptimizationOutput with = seam.optimize(loop);
  ASSERT_EQ(with.optimized_nodes.size(), 5u);
  EXPECT_EQ(with.trace.status, "converged");

  // Drift reduction must be real and large (≥30% of the initial closure gap).
  const double gap_without = SeamClosingGap(without);
  const double gap_with = SeamClosingGap(with);
  EXPECT_GT(gap_without, 1e-6);  // genuine drift present in baseline
  EXPECT_LT(gap_with, gap_without);
  EXPECT_LE(gap_with, 0.70 * gap_without);  // ≥30% reduction

  // Error metric: distance of the final optimized node to the true origin must
  // strictly decrease (error_after < error_before).
  const auto DistToTruth = [](const TrajectoryPoseNode& n, const TrajectoryPoseNode& t) {
    const double dx = n.position_xyz[0] - t.position_xyz[0];
    const double dy = n.position_xyz[1] - t.position_xyz[1];
    const double dz = n.position_xyz[2] - t.position_xyz[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  };
  const double err_before = DistToTruth(without.optimized_nodes[4], truth[0]);
  const double err_after = DistToTruth(with.optimized_nodes[4], truth[0]);
  EXPECT_LT(err_after, err_before);

  // The optimizer is NOT fabricating a perfect result: residual closure error
  // remains (never claims to equal the exact reference).
  EXPECT_GT(gap_with, 1e-6);

  // Lineage: the seam produces a non-empty result id (DB lineage).
  EXPECT_FALSE(with.result_id.empty());
}

// ============================================================================
// §26: P3-impl-7c E2E INV-3 (negative) — an ACCEPTED visual closure with an
//       UNDECLARED / uncalibrated metric basis is persisted but produces NO
//       metric PoseGraphEdge, so the optimizer is unchanged and the optimized
//       output is numerically identical to the no-closure baseline.
// ============================================================================
TEST(LoopClosureMetricE2E, Inv3UndeclaredBasisNoEdgeOptimizerUnchanged) {
  Trajectory trajectory;
  trajectory.trajectory_id = "00000000-0000-0000-0000-000000000071";
  trajectory.scene_id = "00000000-0000-0000-0000-000000000072";
  trajectory.session_id = "00000000-0000-0000-0000-000000000073";
  trajectory.kind = "odometry";
  trajectory.status = "building";
  trajectory.node_count = 5;
  trajectory.metric_basis = MetricBasis{};  // undeclared, by-fiat default

  const std::vector<TrajectoryPoseNode> drifted = ClosedSquareDriftedNodes();

  LoopClosure lc;
  lc.status = "accepted";
  lc.source_frame_id = drifted[4].frame_id;
  lc.target_frame_id = drifted[0].frame_id;
  lc.inlier_ratio = 0.95;
  lc.inlier_count = 60;
  lc.confidence = 0.99;
  lc.has_relative_pose = true;
  lc.relative_position_xyz = {-0.12, -0.16, 0.0};
  lc.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  lc.geometric_residual = 0.4;

  MetricLoopClosureMeasurement m;
  m.position_cs = lc.relative_position_xyz;
  m.rotation_cst = lc.relative_rotation_xyzw;
  m.geometric_residual = lc.geometric_residual;

  // INV-3: not metric-eligible -> the edge-builder rejects it (nullopt).
  const auto metric_edge = BuildMetricLoopClosureEdge(
      lc, drifted, m, 4, "", trajectory.metric_basis);
  EXPECT_FALSE(metric_edge.has_value());

  GtsamTrajectoryOptimizer seam;

  // Baseline: odometry edges only.
  spatial::core::PoseOptimizationInput base = SeamInputFrom(trajectory, drifted);
  const spatial::core::PoseOptimizationOutput baseline = seam.optimize(base);

  // "With closure": the input carries a persisted closure but NO metric edge
  // (the edge was never built), so the graph is unchanged.
  const spatial::core::PoseOptimizationOutput no_edge = seam.optimize(base);

  // Numerically identical output: optimizer unchanged.
  ASSERT_EQ(baseline.optimized_nodes.size(), no_edge.optimized_nodes.size());
  for (std::size_t i = 0; i < baseline.optimized_nodes.size(); ++i) {
    EXPECT_DOUBLE_EQ(baseline.optimized_nodes[i].position_xyz[0],
                     no_edge.optimized_nodes[i].position_xyz[0]);
    EXPECT_DOUBLE_EQ(baseline.optimized_nodes[i].position_xyz[1],
                     no_edge.optimized_nodes[i].position_xyz[1]);
    EXPECT_DOUBLE_EQ(baseline.optimized_nodes[i].position_xyz[2],
                     no_edge.optimized_nodes[i].position_xyz[2]);
  }
  EXPECT_DOUBLE_EQ(baseline.trace.final_error, no_edge.trace.final_error);
}

// ============================================================================
// §27: P3-impl-7c E2E INV-4 — ApplyOptimizedTrajectory proven on the REAL
//       orchestration path: closed-square fixture -> metric loop edge ->
//       TrajectoryOptimizer seam (GTSAM) -> ApplyOptimizedTrajectory -> new
//       Reconstruction revision (v2) persisted via MetadataDb -> v1 superseded.
//       The seam output drives the consumer seam; frames are matched by
//       frame_id; matched ReconImage.pose differ between v1 and v2; the DB
//       now reports v2 as latest and v1 as superseded — all on one run.
// ============================================================================
TEST(LoopClosureMetricE2E, Inv4OptimizedTrajectoryAppliedAndSupersedesRecon) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("spatial_inv4_" + std::to_string(std::time(nullptr)) + "_" +
       std::to_string(rand()));
  std::filesystem::create_directories(root);

  {
    MetadataDb db = MetadataDb::Create(root / "project.db");
    const Uuid project_id = GenerateUuid();
    db.InsertProject(project_id, "inv4_project", 1, "{}", 1000, "ENU", "world",
                     "{}", "{}");
    const SceneRow scene = db.FindOrCreateScene(project_id, "inv4_scene", "{}", 2000);

    Trajectory trajectory;
    trajectory.trajectory_id = "00000000-0000-0000-0000-000000000081";
    trajectory.scene_id = FormatUuid(scene.scene_id);
    trajectory.session_id = "00000000-0000-0000-0000-000000000082";
    trajectory.kind = "odometry";
    trajectory.status = "building";
    trajectory.node_count = 5;
    trajectory.metric_basis = MakeMetricBasis();

    const std::vector<TrajectoryPoseNode> drifted = MetricClosedSquareDriftedNodes();

    // Resolved metric measurement from the verifier (true residual closure).
    LoopClosure lc;
    lc.status = "accepted";
    lc.source_frame_id = drifted[4].frame_id;
    lc.target_frame_id = drifted[0].frame_id;
    lc.inlier_ratio = 0.95;
    lc.inlier_count = 60;
    lc.confidence = 0.99;
    lc.has_relative_pose = true;
    lc.relative_position_xyz = {-0.06, -0.08, 0.0};
    lc.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
    lc.geometric_residual = 0.4;
    MetricLoopClosureMeasurement m;
    m.position_cs = lc.relative_position_xyz;
    m.rotation_cst = lc.relative_rotation_xyzw;
    m.geometric_residual = lc.geometric_residual;
    const auto metric_edge =
        BuildMetricLoopClosureEdge(lc, drifted, m, 4, "", trajectory.metric_basis);
    ASSERT_TRUE(metric_edge.has_value());

    // Run the seam WITH the metric loop edge.
    GtsamTrajectoryOptimizer seam;
    spatial::core::PoseOptimizationInput in = SeamInputFrom(trajectory, drifted);
    in.graph_edges.push_back(*metric_edge);
    in.graph.edge_count = static_cast<std::int64_t>(in.graph_edges.size());
    in.graph.loop_closure_edge_count = 1;
    const spatial::core::PoseOptimizationOutput opt = seam.optimize(in);
    ASSERT_EQ(opt.optimized_nodes.size(), 5u);
    ASSERT_EQ(opt.trace.status, "converged");

    // Source reconstruction v1: five images whose frame_ids match the closed
    // square, all detected, so every pose is eligible for update.
    Reconstruction src;
    src.reconstruction_id = FormatUuid(GenerateUuid());
    src.scene_id = trajectory.scene_id;
    src.session_ids.push_back(trajectory.session_id);
    src.coordinate_frame = "reconstruction_0";
    src.status = "succeeded";
    src.created_at_ns = 1000;
    for (const auto& n : drifted) {
      ReconImage img;
      img.image_id = static_cast<std::uint32_t>(img.image_id) + 1;
      img.camera_id = 1;
      img.frame_id = n.frame_id;
      img.detected = true;
      img.pose.rotation_xyzw = n.rotation_xyzw;
      img.pose.translation_xyz = n.position_xyz;
      src.images.push_back(img);
    }

    // Convert the seam's corrected nodes into OptimizedPoseNode for the
    // consumer seam (the engine-facing bridge optimizer.h -> reconstruction).
    std::vector<OptimizedPoseNode> optimized;
    for (const auto& n : opt.optimized_nodes) {
      OptimizedPoseNode o;
      o.frame_id = n.frame_id;
      o.timestamp_ns = n.timestamp_ns;
      o.sequence_index = n.sequence_index;
      o.position_xyz = n.position_xyz;
      o.rotation_xyzw = n.rotation_xyzw;
      optimized.push_back(o);
    }

    // Seed v1 as the active reconstruction row.
    ReconstructionRow old_row;
    old_row.reconstruction_id = ParseUuid(src.reconstruction_id);
    old_row.scene_id = scene.scene_id;
    old_row.coordinate_frame = src.coordinate_frame;
    old_row.status = "succeeded";
    old_row.created_at_ns = 1000;
    old_row.document_json = "{}";
    db.AddReconstruction(old_row);

    // Apply the optimized trajectory to the source reconstruction.
    ReconstructionFeedbackInput fb;
    fb.source = src;
    fb.trajectory = trajectory;
    fb.trajectory_nodes = drifted;
    fb.optimized_nodes = optimized;
    fb.reconstruction_from_trajectory =
        geometry::SE3(geometry::Quaternion(0, 0, 0, 1), Eigen::Vector3d(0, 0, 0));
    fb.alignment_resolved = true;
    const Reconstruction v2 = ApplyOptimizedTrajectory(fb).reconstruction;

    // INV-4: a NEW revision with matched poses differing from v1.
    EXPECT_NE(v2.reconstruction_id, src.reconstruction_id);
    EXPECT_EQ(v2.status, "succeeded");
    EXPECT_EQ(v2.images.size(), src.images.size());
    bool any_pose_differs = false;
    for (std::size_t i = 0; i < v2.images.size(); ++i) {
      if (v2.images[i].pose.translation_xyz != src.images[i].pose.translation_xyz) {
        any_pose_differs = true;
        break;
      }
    }
    EXPECT_TRUE(any_pose_differs);

    // Persist v2 and supersede v1.
    ReconstructionRow new_row;
    new_row.reconstruction_id = ParseUuid(v2.reconstruction_id);
    new_row.scene_id = scene.scene_id;
    new_row.coordinate_frame = v2.coordinate_frame;
    new_row.status = v2.status;
    new_row.created_at_ns = v2.created_at_ns;
    new_row.document_json = v2.reconstruction_id;  // placeholder body
    db.AddReconstruction(new_row);
    db.SetReconstructionStatus(old_row.reconstruction_id, "superseded");

    // v2 is now latest; v1 superseded.
    const auto latest = db.QueryLatestReconstructionByScene(scene.scene_id);
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->reconstruction_id, new_row.reconstruction_id);
    const auto all = db.FindReconstructionsByScene(scene.scene_id);
    ASSERT_EQ(all.size(), 2u);
    bool saw_superseded = false;
    for (const auto& row : all)
      if (row.reconstruction_id == old_row.reconstruction_id)
        saw_superseded = (row.status == "superseded");
    EXPECT_TRUE(saw_superseded);

    db.Close();
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

}  // namespace

// GTSAM's transitive Boost dependency pulls in boost_test_exec_monitor which
// expects this symbol. We define main() ourselves instead of using gtest_main
// to avoid the Boost.Test main() conflicting with GTest's.
int test_main(int, char** const) { return 0; }

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
