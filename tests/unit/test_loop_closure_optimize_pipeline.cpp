// P3-impl-7 Phase 3 — orchestration-runner (DB persistence increment) tests.
//
// Proves the REAL engine orchestration path writes the canonical rows:
//   Verified LoopClosure -> PoseGraphRow -> (seam) OptimizationResultRow,
// with idempotent (INSERT OR REPLACE) persistence and the real UPDATE path for
// LoopClosure.spatial_separation_m. Also proves INV-3 (undeclared basis -> no
// pose graph, no optimization, closure still persisted for audit).
//
// Engine code never sees GTSAM; the optimizer is injected through the
// core::TrajectoryOptimizer seam (here backed by the real GTSAM adapter).

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "adapters/gtsam/gtsam_optimizer_adapter.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/optimizer.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/loop_closure_optimize_pipeline.h"

namespace {

using spatial::core::LoopClosure;
using spatial::core::MetadataDb;
using spatial::core::MetricBasis;
using spatial::core::MetricBasisSource;
using spatial::core::MetricBasisType;
using spatial::core::Trajectory;
using spatial::core::TrajectoryPoseNode;
using spatial::core::Uuid;
using spatial::engine::LoopClosureOptimizePipeline;
using spatial::engine::LoopClosureOptimizePipelineInput;
using spatial::engine::LoopClosureOptimizePipelineResult;

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

// Drifted closed square with a small TRUE residual closure offset at node 4
// (so the verifier's metric measurement is real, non-identity, INV-1):
//   P0 origin, P4 = (0.31, 0.33).
std::vector<TrajectoryPoseNode> DriftedSquare() {
  return {
      MakeNode(0, "00000000-0000-0000-0000-0000000000F0", 0.0, 0.0, 0.0),
      MakeNode(1, "00000000-0000-0000-0000-0000000000F1", 1.02, 0.0, 0.0),
      MakeNode(2, "00000000-0000-0000-0000-0000000000F2", 1.02, 1.02, 0.0),
      MakeNode(3, "00000000-0000-0000-0000-0000000000F3", 0.01, 1.02, 0.0),
      MakeNode(4, "00000000-0000-0000-0000-0000000000F4", 0.31, 0.33, 0.0)};
}

// Accepted metric closure: source = node 4, target = node 0, with the verifier's
// resolved metric relative pose (-0.06, -0.08) (true residual closure).
LoopClosure MakeClosure(const std::vector<TrajectoryPoseNode>& nodes,
                        const std::string& trajectory_id,
                        const std::string& closure_id) {
  LoopClosure lc;
  lc.closure_id = closure_id;
  lc.trajectory_id = trajectory_id;
  lc.candidate_id = "00000000-0000-0000-0000-0000000000CA";
  lc.source_frame_id = nodes[4].frame_id;
  lc.target_frame_id = nodes[0].frame_id;
  lc.status = "accepted";
  lc.inlier_ratio = 0.95;
  lc.inlier_count = 60;
  lc.confidence = 0.99;
  lc.temporal_separation_ns = 40;
  lc.created_at_ns = 100;
  lc.has_relative_pose = true;
  lc.relative_position_xyz = {-0.06, -0.08, 0.0};
  lc.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  lc.geometric_residual = 0.4;
  return lc;
}

class LoopClosureOptimizePipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_lcopt_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_.emplace(MetadataDb::Create(root_ / "project.db"));
    project_id_ = spatial::core::GenerateUuid();
    db_->InsertProject(project_id_, "lcopt_project", 1, "{}", 1000, "ENU",
                       "world", "{}", "{}");
    scene_ = db_->FindOrCreateScene(project_id_, "lcopt_scene", "{}", 2000);

    trajectory_.trajectory_id = "00000000-0000-0000-0000-0000000000E1";
    trajectory_.scene_id = spatial::core::FormatUuid(scene_.scene_id);
    trajectory_.session_id = "00000000-0000-0000-0000-0000000000E2";
    trajectory_.kind = "odometry";
    trajectory_.status = "building";
    trajectory_.node_count = 5;
    trajectory_.created_at_ns = 100;
    trajectory_.metric_basis = MakeBasis();

    nodes_ = DriftedSquare();
  }

  void TearDown() override {
    if (db_) db_->Close();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  Uuid project_id_{};
  spatial::core::SceneRow scene_{};
  Trajectory trajectory_;
  std::vector<TrajectoryPoseNode> nodes_;
};

// Positive persistence path: closure -> PoseGraphRow -> OptimizationResultRow,
// spatial_separation_m persisted, and deterministic graph_id/result_id.
TEST_F(LoopClosureOptimizePipelineTest, PersistsGraphOptAndClosureSpatial) {
  LoopClosure closure = MakeClosure(nodes_, trajectory_.trajectory_id,
                                    "00000000-0000-0000-0000-0000000000C1");

  spatial::adapters::gtsam::GtsamTrajectoryOptimizer seam;

  LoopClosureOptimizePipelineInput in;
  in.trajectory = &trajectory_;
  in.trajectory_nodes = &nodes_;
  in.closure = closure;
  in.metric_basis = MakeBasis();
  in.configuration_hash = "cfg-hash";
  in.optimizer = &seam;
  in.db = &*db_;

  const LoopClosureOptimizePipelineResult out = LoopClosureOptimizePipeline(in);
  ASSERT_TRUE(out.ran);
  EXPECT_TRUE(out.metric_eligible);
  EXPECT_EQ(out.optimized.size(), 5u);
  ASSERT_TRUE(out.optimization.has_value());
  EXPECT_EQ(out.optimization->status, "converged");

  // spatial_separation_m = ||p_target - p_source|| = ||(0,0) - (0.31,0.33)||.
  const double spatial = out.spatial_separation_m;
  EXPECT_NEAR(spatial, std::sqrt(0.31 * 0.31 + 0.33 * 0.33), 1e-9);

  // PoseGraph row persisted via the real engine path.
  const auto graphs = db_->FindPoseGraphsByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  ASSERT_EQ(graphs.size(), 1u);
  EXPECT_EQ(graphs[0].graph_id, spatial::core::ParseUuid(out.graph_id));
  EXPECT_EQ(graphs[0].loop_closure_edge_count, 1);
  EXPECT_EQ(graphs[0].odometry_edge_count, 4);
  EXPECT_EQ(graphs[0].edge_count, 5);

  const auto graphs_latest = db_->QueryLatestPoseGraphByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  ASSERT_TRUE(graphs_latest.has_value());
  EXPECT_EQ(graphs_latest->graph_id, spatial::core::ParseUuid(out.graph_id));

  // LoopClosure row persisted with the D2 spatial_separation_m filled.
  const auto closures = db_->FindLoopClosuresByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  ASSERT_EQ(closures.size(), 1u);
  EXPECT_NEAR(closures[0].spatial_separation_m, spatial, 1e-9);
  EXPECT_EQ(closures[0].status, "accepted");

  // OptimizationResult row persisted.
  const auto opts = db_->FindOptimizationResultsByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  ASSERT_EQ(opts.size(), 1u);
  EXPECT_EQ(opts[0].result_id, spatial::core::ParseUuid(out.result_id));
  EXPECT_EQ(opts[0].graph_id, spatial::core::ParseUuid(out.graph_id));
  EXPECT_EQ(opts[0].status, "converged");
}

// Idempotency: re-processing the SAME verified closure (same closure_id /
// trajectory) must NOT create duplicate or contradictory canonical rows. It
// must overwrite the existing closure/PoseGraph/OptimizationResult rows, keep
// the stable graph_id/result_id, and never throw a PK-conflict.
TEST_F(LoopClosureOptimizePipelineTest, RepeatedProcessingIsIdempotent) {
  LoopClosure closure = MakeClosure(nodes_, trajectory_.trajectory_id,
                                    "00000000-0000-0000-0000-0000000000C2");

  spatial::adapters::gtsam::GtsamTrajectoryOptimizer seam;
  LoopClosureOptimizePipelineInput in;
  in.trajectory = &trajectory_;
  in.trajectory_nodes = &nodes_;
  in.closure = closure;
  in.metric_basis = MakeBasis();
  in.configuration_hash = "cfg-hash";
  in.optimizer = &seam;
  in.db = &*db_;

  const LoopClosureOptimizePipelineResult first = LoopClosureOptimizePipeline(in);
  ASSERT_TRUE(first.ran);

  // Re-process the identical closure+trajectory (same closure_id).
  const LoopClosureOptimizePipelineResult second = LoopClosureOptimizePipeline(in);
  ASSERT_TRUE(second.ran);

  // Stable idempotent identities across runs.
  EXPECT_EQ(first.graph_id, second.graph_id);
  EXPECT_EQ(first.result_id, second.result_id);

  // Still exactly ONE canonical row of each kind (no duplicates, no conflicts).
  const auto graphs = db_->FindPoseGraphsByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  ASSERT_EQ(graphs.size(), 1u);
  EXPECT_EQ(graphs[0].graph_id, spatial::core::ParseUuid(first.graph_id));

  const auto opts = db_->FindOptimizationResultsByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  ASSERT_EQ(opts.size(), 1u);
  EXPECT_EQ(opts[0].result_id, spatial::core::ParseUuid(first.result_id));

  const auto closures = db_->FindLoopClosuresByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  ASSERT_EQ(closures.size(), 1u);
  EXPECT_EQ(closures[0].closure_id,
            spatial::core::ParseUuid(closure.closure_id));
  EXPECT_NEAR(closures[0].spatial_separation_m, first.spatial_separation_m,
              1e-9);
}

// INV-3 (negative): undeclared metric basis -> NO pose graph, NO optimization,
// but the closure is still persisted (verified-visual-only audit), and the run
// reports ran=false without throwing.
TEST_F(LoopClosureOptimizePipelineTest, UndeclaredBasisNoOptButClosurePersisted) {
  LoopClosure closure = MakeClosure(nodes_, trajectory_.trajectory_id,
                                    "00000000-0000-0000-0000-0000000000C3");

  spatial::adapters::gtsam::GtsamTrajectoryOptimizer seam;
  LoopClosureOptimizePipelineInput in;
  in.trajectory = &trajectory_;
  in.trajectory_nodes = &nodes_;
  in.closure = closure;
  in.metric_basis = MetricBasis{};  // undeclared (INV-3)
  in.configuration_hash = "cfg-hash";
  in.optimizer = &seam;
  in.db = &*db_;

  const LoopClosureOptimizePipelineResult out = LoopClosureOptimizePipeline(in);
  EXPECT_FALSE(out.ran);
  EXPECT_FALSE(out.metric_eligible);
  EXPECT_FALSE(out.optimization.has_value());

  // Closure persisted for audit with D2 spatial_separation_m filled.
  const auto closures = db_->FindLoopClosuresByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  ASSERT_EQ(closures.size(), 1u);
  EXPECT_NEAR(closures[0].spatial_separation_m, out.spatial_separation_m, 1e-9);

  // No pose graph, no optimization were manufactured (nothing fabricated).
  const auto graphs = db_->FindPoseGraphsByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  EXPECT_TRUE(graphs.empty());
  const auto opts = db_->FindOptimizationResultsByTrajectory(
      spatial::core::ParseUuid(trajectory_.trajectory_id));
  EXPECT_TRUE(opts.empty());
}

}  // namespace

// Mirror the gtsam-adapter test pattern: GTSAM's transitive Boost dependency
// pulls in boost_test_exec_monitor, which expects test_main(); provide main()
// ourselves rather than using gtest_main (which conflicts with Boost.Test).
int test_main(int, char** const) { return 0; }

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
