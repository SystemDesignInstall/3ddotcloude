#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include "core/trajectory/trajectory.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/optimization.h"
#include "core/errors/project_error.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"

namespace spatial::core {
namespace {

// ==================== Domain Type Tests ====================

// --- TrajectoryUncertainty ---

TEST(TrajectoryUncertainty, DefaultConstruction) {
  TrajectoryUncertainty u;
  EXPECT_DOUBLE_EQ(u.mean_position_uncertainty_m, 0.0);
  EXPECT_DOUBLE_EQ(u.max_position_uncertainty_m, 0.0);
  EXPECT_DOUBLE_EQ(u.mean_rotation_uncertainty_rad, 0.0);
  EXPECT_DOUBLE_EQ(u.max_rotation_uncertainty_rad, 0.0);
  EXPECT_DOUBLE_EQ(u.loop_closure_density, 0.0);
}

TEST(TrajectoryUncertainty, Equality) {
  TrajectoryUncertainty a;
  a.mean_position_uncertainty_m = 0.05;
  a.max_position_uncertainty_m = 0.2;
  a.mean_rotation_uncertainty_rad = 0.01;
  a.max_rotation_uncertainty_rad = 0.05;
  a.loop_closure_density = 2.5;

  TrajectoryUncertainty b = a;
  EXPECT_EQ(a, b);

  TrajectoryUncertainty c = a;
  c.loop_closure_density = 3.0;
  EXPECT_NE(a, c);
}

// --- TrajectoryPoseNode ---

TEST(TrajectoryPoseNode, DefaultConstruction) {
  TrajectoryPoseNode node;
  EXPECT_TRUE(node.frame_id.empty());
  EXPECT_EQ(node.timestamp_ns, 0);
  EXPECT_EQ(node.sequence_index, 0);
  EXPECT_DOUBLE_EQ(node.position_xyz[0], 0.0);
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[3], 0.0);
  EXPECT_DOUBLE_EQ(node.covariance_position[0], 0.0);
}

TEST(TrajectoryPoseNode, Equality) {
  TrajectoryPoseNode a;
  a.frame_id = "550e8400-e29b-41d4-a716-446655440000";
  a.timestamp_ns = 1783123200000000000LL;
  a.sequence_index = 42;
  a.position_xyz = {1.0, 2.0, 3.0};
  a.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  a.covariance_position = {0.01, 0.0, 0.0, 0.01, 0.0, 0.01};

  TrajectoryPoseNode b = a;
  EXPECT_EQ(a, b);

  TrajectoryPoseNode c = a;
  c.sequence_index = 43;
  EXPECT_NE(a, c);
}

// --- Trajectory ---

TEST(Trajectory, DefaultConstruction) {
  Trajectory traj;
  EXPECT_TRUE(traj.trajectory_id.empty());
  EXPECT_TRUE(traj.scene_id.empty());
  EXPECT_TRUE(traj.session_id.empty());
  EXPECT_TRUE(traj.kind.empty());
  EXPECT_TRUE(traj.coordinate_frame.empty());
  EXPECT_TRUE(traj.status.empty());
  EXPECT_EQ(traj.created_at_ns, 0);
  EXPECT_EQ(traj.node_count, 0);
  EXPECT_DOUBLE_EQ(traj.total_distance_m, 0.0);
  EXPECT_DOUBLE_EQ(traj.total_duration_ns, 0.0);
}

TEST(Trajectory, FullConstruction) {
  Trajectory traj;
  traj.trajectory_id = "550e8400-e29b-41d4-a716-446655440000";
  traj.scene_id = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
  traj.session_id = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
  traj.kind = "slam";
  traj.coordinate_frame = "trajectory_0";
  traj.status = "optimized";
  traj.created_at_ns = 1234567890;
  traj.node_count = 100;
  traj.total_distance_m = 50.5;
  traj.total_duration_ns = 30000000000LL;

  traj.uncertainty.mean_position_uncertainty_m = 0.05;
  traj.uncertainty.loop_closure_density = 2.0;

  traj.provenance.backend.name = "orbslam3";
  traj.provenance.backend.version = "3.0";

  EXPECT_EQ(traj.trajectory_id, "550e8400-e29b-41d4-a716-446655440000");
  EXPECT_EQ(traj.kind, "slam");
  EXPECT_EQ(traj.status, "optimized");
  EXPECT_EQ(traj.node_count, 100);
  EXPECT_EQ(traj.provenance.backend.name, "orbslam3");
}

TEST(Trajectory, Equality) {
  Trajectory a;
  a.trajectory_id = "550e8400-e29b-41d4-a716-446655440000";
  a.status = "optimized";

  Trajectory b = a;
  EXPECT_EQ(a, b);

  Trajectory c = a;
  c.status = "superseded";
  EXPECT_NE(a, c);
}

// --- PoseGraphNode ---

TEST(PoseGraphNode, DefaultConstruction) {
  PoseGraphNode node;
  EXPECT_EQ(node.node_id, 0);
  EXPECT_TRUE(node.frame_id.empty());
  EXPECT_EQ(node.timestamp_ns, 0);
}

TEST(PoseGraphNode, Equality) {
  PoseGraphNode a;
  a.node_id = 5;
  a.frame_id = "550e8400-e29b-41d4-a716-446655440000";
  a.timestamp_ns = 1000;

  PoseGraphNode b = a;
  EXPECT_EQ(a, b);

  PoseGraphNode c = a;
  c.node_id = 6;
  EXPECT_NE(a, c);
}

// --- PoseGraphEdge ---

TEST(PoseGraphEdge, DefaultConstruction) {
  PoseGraphEdge edge;
  EXPECT_EQ(edge.edge_id, 0);
  EXPECT_TRUE(edge.type.empty());
  EXPECT_EQ(edge.source_node_id, 0);
  EXPECT_EQ(edge.target_node_id, 0);
  EXPECT_DOUBLE_EQ(edge.confidence, 0.0);
}

TEST(PoseGraphEdge, Equality) {
  PoseGraphEdge a;
  a.edge_id = 1;
  a.type = "odometry";
  a.source_node_id = 0;
  a.target_node_id = 1;
  a.relative_position_xyz = {0.1, 0.0, 0.0};
  a.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  a.confidence = 0.95;
  a.source = "visual_odometry";

  PoseGraphEdge b = a;
  EXPECT_EQ(a, b);

  PoseGraphEdge c = a;
  c.type = "loop_closure";
  EXPECT_NE(a, c);
}

TEST(PoseGraphEdge, InformationMatrix) {
  PoseGraphEdge edge;
  // Identity information matrix (36 elements, row-major 6x6)
  edge.information_matrix_6x6[0] = 1000.0;   // xx
  edge.information_matrix_6x6[7] = 1000.0;   // yy
  edge.information_matrix_6x6[14] = 1000.0;  // zz
  edge.information_matrix_6x6[21] = 100.0;   // rx rx
  edge.information_matrix_6x6[28] = 100.0;   // ry ry
  edge.information_matrix_6x6[35] = 100.0;   // rz rz

  EXPECT_DOUBLE_EQ(edge.information_matrix_6x6[0], 1000.0);
  EXPECT_DOUBLE_EQ(edge.information_matrix_6x6[35], 100.0);
}

// --- PoseGraphConstraint ---

TEST(PoseGraphConstraint, DefaultConstruction) {
  PoseGraphConstraint c;
  EXPECT_EQ(c.source_node_id, 0);
  EXPECT_EQ(c.target_node_id, 0);
  EXPECT_TRUE(c.type.empty());
  EXPECT_DOUBLE_EQ(c.confidence, 0.0);
}

// --- PoseGraph ---

TEST(PoseGraph, DefaultConstruction) {
  PoseGraph pg;
  EXPECT_TRUE(pg.graph_id.empty());
  EXPECT_TRUE(pg.trajectory_id.empty());
  EXPECT_TRUE(pg.scene_id.empty());
  EXPECT_TRUE(pg.status.empty());
  EXPECT_EQ(pg.node_count, 0);
  EXPECT_EQ(pg.edge_count, 0);
  EXPECT_EQ(pg.odometry_edge_count, 0);
  EXPECT_EQ(pg.loop_closure_edge_count, 0);
  EXPECT_EQ(pg.prior_edge_count, 0);
}

TEST(PoseGraph, FullConstruction) {
  PoseGraph pg;
  pg.graph_id = "550e8400-e29b-41d4-a716-446655440000";
  pg.trajectory_id = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
  pg.scene_id = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
  pg.status = "optimized";
  pg.node_count = 100;
  pg.edge_count = 150;
  pg.odometry_edge_count = 99;
  pg.loop_closure_edge_count = 40;
  pg.prior_edge_count = 11;

  EXPECT_EQ(pg.node_count, 100);
  EXPECT_EQ(pg.edge_count, 150);
  EXPECT_EQ(pg.loop_closure_edge_count, 40);
}

TEST(PoseGraph, Equality) {
  PoseGraph a;
  a.graph_id = "550e8400-e29b-41d4-a716-446655440000";
  a.status = "optimized";

  PoseGraph b = a;
  EXPECT_EQ(a, b);

  PoseGraph c = a;
  c.status = "failed";
  EXPECT_NE(a, c);
}

// --- LoopClosureCandidate ---

TEST(LoopClosureCandidate, DefaultConstruction) {
  LoopClosureCandidate cand;
  EXPECT_TRUE(cand.candidate_id.empty());
  EXPECT_TRUE(cand.trajectory_id.empty());
  EXPECT_TRUE(cand.source_frame_id.empty());
  EXPECT_TRUE(cand.target_frame_id.empty());
  EXPECT_DOUBLE_EQ(cand.feature_match_score, 0.0);
  EXPECT_TRUE(cand.matcher.empty());
  EXPECT_EQ(cand.created_at_ns, 0);
}

TEST(LoopClosureCandidate, Equality) {
  LoopClosureCandidate a;
  a.candidate_id = "550e8400-e29b-41d4-a716-446655440000";
  a.source_frame_id = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
  a.target_frame_id = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
  a.feature_match_score = 0.85;
  a.matcher = "bow";

  LoopClosureCandidate b = a;
  EXPECT_EQ(a, b);

  LoopClosureCandidate c = a;
  c.matcher = "netvlad";
  EXPECT_NE(a, c);
}

// --- LoopClosure ---

TEST(LoopClosure, DefaultConstruction) {
  LoopClosure lc;
  EXPECT_TRUE(lc.closure_id.empty());
  EXPECT_TRUE(lc.status.empty());
  EXPECT_DOUBLE_EQ(lc.inlier_ratio, 0.0);
  EXPECT_EQ(lc.inlier_count, 0);
  EXPECT_DOUBLE_EQ(lc.confidence, 0.0);
  EXPECT_EQ(lc.temporal_separation_ns, 0);
  EXPECT_DOUBLE_EQ(lc.spatial_separation_m, 0.0);
}

TEST(LoopClosure, Equality) {
  LoopClosure a;
  a.closure_id = "550e8400-e29b-41d4-a716-446655440000";
  a.status = "accepted";
  a.inlier_ratio = 0.92;
  a.inlier_count = 150;
  a.confidence = 0.95;

  LoopClosure b = a;
  EXPECT_EQ(a, b);

  LoopClosure c = a;
  c.status = "rejected";
  EXPECT_NE(a, c);
}

// --- OptimizationProvenance ---

TEST(OptimizationProvenance, DefaultConstruction) {
  OptimizationProvenance prov;
  EXPECT_TRUE(prov.optimizer.name.empty());
  EXPECT_TRUE(prov.optimizer.version.empty());
  EXPECT_TRUE(prov.configuration_hash.empty());
  EXPECT_TRUE(prov.input_artifact_hashes.empty());
}

TEST(OptimizationProvenance, Equality) {
  OptimizationProvenance a;
  a.optimizer.name = "gtsam";
  a.optimizer.version = "4.2";
  a.configuration_hash = "abc123";

  OptimizationProvenance b = a;
  EXPECT_EQ(a, b);

  OptimizationProvenance c = a;
  c.optimizer.version = "4.3";
  EXPECT_NE(a, c);
}

// --- OptimizedPoseNode ---

TEST(OptimizedPoseNode, DefaultConstruction) {
  OptimizedPoseNode node;
  EXPECT_TRUE(node.frame_id.empty());
  EXPECT_EQ(node.timestamp_ns, 0);
  EXPECT_EQ(node.sequence_index, 0);
  EXPECT_DOUBLE_EQ(node.position_xyz[0], 0.0);
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[3], 0.0);
}

TEST(OptimizedPoseNode, Equality) {
  OptimizedPoseNode a;
  a.frame_id = "550e8400-e29b-41d4-a716-446655440000";
  a.timestamp_ns = 1000;
  a.position_xyz = {1.0, 2.0, 3.0};
  a.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};

  OptimizedPoseNode b = a;
  EXPECT_EQ(a, b);

  OptimizedPoseNode c = a;
  c.position_xyz[0] = 99.0;
  EXPECT_NE(a, c);
}

// --- OptimizationResult ---

TEST(OptimizationResult, DefaultConstruction) {
  OptimizationResult res;
  EXPECT_TRUE(res.result_id.empty());
  EXPECT_TRUE(res.graph_id.empty());
  EXPECT_TRUE(res.trajectory_id.empty());
  EXPECT_TRUE(res.status.empty());
  EXPECT_EQ(res.iterations, 0);
  EXPECT_DOUBLE_EQ(res.initial_error, 0.0);
  EXPECT_DOUBLE_EQ(res.final_error, 0.0);
  EXPECT_DOUBLE_EQ(res.error_reduction, 0.0);
}

TEST(OptimizationResult, FullConstruction) {
  OptimizationResult res;
  res.result_id = "550e8400-e29b-41d4-a716-446655440000";
  res.graph_id = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
  res.trajectory_id = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
  res.status = "converged";
  res.iterations = 50;
  res.initial_error = 1000.0;
  res.final_error = 10.0;
  res.error_reduction = 0.99;

  res.provenance.optimizer.name = "gtsam";
  res.provenance.optimizer.version = "4.2";

  EXPECT_EQ(res.status, "converged");
  EXPECT_EQ(res.iterations, 50);
  EXPECT_DOUBLE_EQ(res.error_reduction, 0.99);
  EXPECT_EQ(res.provenance.optimizer.name, "gtsam");
}

TEST(OptimizationResult, Equality) {
  OptimizationResult a;
  a.result_id = "550e8400-e29b-41d4-a716-446655440000";
  a.status = "converged";

  OptimizationResult b = a;
  EXPECT_EQ(a, b);

  OptimizationResult c = a;
  c.status = "failed";
  EXPECT_NE(a, c);
}

// ==================== Database Tests ====================

class TrajectoryDbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_traj_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    path_ = root_ / "project.db";
    db_ = MetadataDb::Create(path_);
    project_id_ = GenerateUuid();
    db_.InsertProject(project_id_, "test_project", 1, "{}", 1000, "ENU",
                      "world", "{}", "{}");
    scene_ = db_.FindOrCreateScene(project_id_, "test_scene", "{}", 2000);
  }

  void TearDown() override {
    db_.Close();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
  std::filesystem::path path_;
  MetadataDb db_;
  Uuid project_id_{};
  SceneRow scene_{};
};

// --- Migration ---

TEST_F(TrajectoryDbTest, Migration0008Applied) {
  // If migration 0008 fails, the fixture SetUp() will throw.
  TrajectoryRow row;
  row.trajectory_id = GenerateUuid();
  row.scene_id = scene_.scene_id;
  row.session_id = GenerateUuid();
  row.kind = "slam";
  row.coordinate_frame = "trajectory_0";
  row.status = "building";
  row.created_at_ns = 1000;
  row.document_json = "{}";
  EXPECT_NO_THROW(db_.AddTrajectory(row));
}

// --- Trajectory CRUD ---

TEST_F(TrajectoryDbTest, InsertAndQueryTrajectory) {
  const auto session_id = GenerateUuid();

  TrajectoryRow t1;
  t1.trajectory_id = GenerateUuid();
  t1.scene_id = scene_.scene_id;
  t1.session_id = session_id;
  t1.kind = "slam";
  t1.coordinate_frame = "trajectory_0";
  t1.status = "building";
  t1.node_count = 100;
  t1.total_distance_m = 50.5;
  t1.total_duration_ns = 30000000000LL;
  t1.created_at_ns = 1000;
  t1.document_json = R"({"trajectory_id":"t1"})";
  db_.AddTrajectory(t1);

  const auto latest = db_.QueryLatestTrajectoryBySession(session_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->trajectory_id, t1.trajectory_id);
  EXPECT_EQ(latest->kind, "slam");
  EXPECT_EQ(latest->status, "building");
  EXPECT_EQ(latest->node_count, 100);
  EXPECT_DOUBLE_EQ(latest->total_distance_m, 50.5);
  EXPECT_EQ(latest->document_json, R"({"trajectory_id":"t1"})");
}

TEST_F(TrajectoryDbTest, QueryLatestTrajectorySkipsSuperseded) {
  const auto session_id = GenerateUuid();

  TrajectoryRow t1;
  t1.trajectory_id = GenerateUuid();
  t1.scene_id = scene_.scene_id;
  t1.session_id = session_id;
  t1.kind = "odometry";
  t1.status = "superseded";
  t1.created_at_ns = 1000;
  db_.AddTrajectory(t1);

  TrajectoryRow t2;
  t2.trajectory_id = GenerateUuid();
  t2.scene_id = scene_.scene_id;
  t2.session_id = session_id;
  t2.kind = "slam";
  t2.status = "optimized";
  t2.created_at_ns = 2000;
  db_.AddTrajectory(t2);

  const auto latest = db_.QueryLatestTrajectoryBySession(session_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->trajectory_id, t2.trajectory_id);
  EXPECT_EQ(latest->status, "optimized");
}

TEST_F(TrajectoryDbTest, QueryLatestTrajectoryReturnsEmpty) {
  const auto session_id = GenerateUuid();
  const auto result = db_.QueryLatestTrajectoryBySession(session_id);
  EXPECT_FALSE(result.has_value());
}

TEST_F(TrajectoryDbTest, FindTrajectoriesByScene) {
  TrajectoryRow t1;
  t1.trajectory_id = GenerateUuid();
  t1.scene_id = scene_.scene_id;
  t1.session_id = GenerateUuid();
  t1.kind = "odometry";
  t1.status = "building";
  t1.created_at_ns = 1000;
  db_.AddTrajectory(t1);

  TrajectoryRow t2;
  t2.trajectory_id = GenerateUuid();
  t2.scene_id = scene_.scene_id;
  t2.session_id = GenerateUuid();
  t2.kind = "slam";
  t2.status = "optimized";
  t2.created_at_ns = 2000;
  db_.AddTrajectory(t2);

  const auto all = db_.FindTrajectoriesByScene(scene_.scene_id);
  EXPECT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].created_at_ns, 1000);
  EXPECT_EQ(all[1].created_at_ns, 2000);
}

TEST_F(TrajectoryDbTest, FindTrajectoriesBySession) {
  const auto session_id = GenerateUuid();

  TrajectoryRow t1;
  t1.trajectory_id = GenerateUuid();
  t1.scene_id = scene_.scene_id;
  t1.session_id = session_id;
  t1.kind = "odometry";
  t1.status = "building";
  t1.created_at_ns = 1000;
  db_.AddTrajectory(t1);

  const auto session_id_b = GenerateUuid();
  TrajectoryRow t2;
  t2.trajectory_id = GenerateUuid();
  t2.scene_id = scene_.scene_id;
  t2.session_id = session_id_b;
  t2.kind = "slam";
  t2.status = "optimized";
  t2.created_at_ns = 2000;
  db_.AddTrajectory(t2);

  const auto result = db_.FindTrajectoriesBySession(session_id);
  EXPECT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].session_id, session_id);
}

TEST_F(TrajectoryDbTest, TrajectorySceneIsolation) {
  const auto project_b = GenerateUuid();
  db_.InsertProject(project_b, "project_b", 1, "{}", 1000, "ENU", "world",
                    "{}", "{}");
  const auto scene_b = db_.FindOrCreateScene(project_b, "scene_b", "{}", 3000);

  TrajectoryRow t1;
  t1.trajectory_id = GenerateUuid();
  t1.scene_id = scene_.scene_id;
  t1.session_id = GenerateUuid();
  t1.kind = "slam";
  t1.status = "building";
  t1.created_at_ns = 1000;
  db_.AddTrajectory(t1);

  TrajectoryRow t2;
  t2.trajectory_id = GenerateUuid();
  t2.scene_id = scene_b.scene_id;
  t2.session_id = GenerateUuid();
  t2.kind = "odometry";
  t2.status = "building";
  t2.created_at_ns = 2000;
  db_.AddTrajectory(t2);

  const auto all_a = db_.FindTrajectoriesByScene(scene_.scene_id);
  EXPECT_EQ(all_a.size(), 1u);

  const auto all_b = db_.FindTrajectoriesByScene(scene_b.scene_id);
  EXPECT_EQ(all_b.size(), 1u);
}

// --- PoseGraph CRUD ---

TEST_F(TrajectoryDbTest, InsertAndQueryPoseGraph) {
  const auto traj_id = Uuid{};  // Use a deterministic UUID for trajectory_id
  TrajectoryRow traj;
  traj.trajectory_id = GenerateUuid();
  traj.scene_id = scene_.scene_id;
  traj.session_id = GenerateUuid();
  traj.kind = "slam";
  traj.status = "building";
  traj.created_at_ns = 1000;
  db_.AddTrajectory(traj);

  PoseGraphRow pg;
  pg.graph_id = GenerateUuid();
  pg.trajectory_id = traj.trajectory_id;
  pg.scene_id = scene_.scene_id;
  pg.status = "ready";
  pg.node_count = 50;
  pg.edge_count = 75;
  pg.odometry_edge_count = 49;
  pg.loop_closure_edge_count = 15;
  pg.prior_edge_count = 11;
  pg.created_at_ns = 2000;
  pg.document_json = R"({"graph_id":"pg1"})";
  db_.AddPoseGraph(pg);

  const auto latest = db_.QueryLatestPoseGraphByTrajectory(traj.trajectory_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->graph_id, pg.graph_id);
  EXPECT_EQ(latest->status, "ready");
  EXPECT_EQ(latest->node_count, 50);
  EXPECT_EQ(latest->edge_count, 75);
  EXPECT_EQ(latest->loop_closure_edge_count, 15);
  EXPECT_EQ(latest->document_json, R"({"graph_id":"pg1"})");
}

TEST_F(TrajectoryDbTest, FindPoseGraphsByTrajectory) {
  TrajectoryRow traj;
  traj.trajectory_id = GenerateUuid();
  traj.scene_id = scene_.scene_id;
  traj.session_id = GenerateUuid();
  traj.kind = "slam";
  traj.status = "building";
  traj.created_at_ns = 1000;
  db_.AddTrajectory(traj);

  PoseGraphRow pg1;
  pg1.graph_id = GenerateUuid();
  pg1.trajectory_id = traj.trajectory_id;
  pg1.scene_id = scene_.scene_id;
  pg1.status = "building";
  pg1.created_at_ns = 2000;
  db_.AddPoseGraph(pg1);

  PoseGraphRow pg2;
  pg2.graph_id = GenerateUuid();
  pg2.trajectory_id = traj.trajectory_id;
  pg2.scene_id = scene_.scene_id;
  pg2.status = "optimized";
  pg2.created_at_ns = 3000;
  db_.AddPoseGraph(pg2);

  const auto all = db_.FindPoseGraphsByTrajectory(traj.trajectory_id);
  EXPECT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].status, "building");
  EXPECT_EQ(all[1].status, "optimized");
}

// --- LoopClosureCandidate CRUD ---

TEST_F(TrajectoryDbTest, InsertAndQueryLoopClosureCandidates) {
  const auto traj_id = GenerateUuid();

  LoopClosureCandidateRow c1;
  c1.candidate_id = GenerateUuid();
  c1.trajectory_id = traj_id;
  c1.source_frame_id = GenerateUuid();
  c1.target_frame_id = GenerateUuid();
  c1.feature_match_score = 0.85;
  c1.matcher = "bow";
  c1.created_at_ns = 1000;
  db_.AddLoopClosureCandidate(c1);

  LoopClosureCandidateRow c2;
  c2.candidate_id = GenerateUuid();
  c2.trajectory_id = traj_id;
  c2.source_frame_id = GenerateUuid();
  c2.target_frame_id = GenerateUuid();
  c2.feature_match_score = 0.72;
  c2.matcher = "netvlad";
  c2.created_at_ns = 2000;
  db_.AddLoopClosureCandidate(c2);

  const auto result = db_.FindLoopClosureCandidatesByTrajectory(traj_id);
  EXPECT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0].matcher, "bow");
  EXPECT_EQ(result[1].matcher, "netvlad");
  EXPECT_DOUBLE_EQ(result[0].feature_match_score, 0.85);
}

// --- LoopClosure CRUD ---

TEST_F(TrajectoryDbTest, InsertAndQueryLoopClosures) {
  const auto traj_id = GenerateUuid();

  LoopClosureRow lc1;
  lc1.closure_id = GenerateUuid();
  lc1.trajectory_id = traj_id;
  lc1.source_frame_id = GenerateUuid();
  lc1.target_frame_id = GenerateUuid();
  lc1.status = "accepted";
  lc1.inlier_ratio = 0.92;
  lc1.inlier_count = 150;
  lc1.confidence = 0.95;
  lc1.temporal_separation_ns = 5000000000LL;
  lc1.spatial_separation_m = 25.3;
  lc1.created_at_ns = 1000;
  db_.AddLoopClosure(lc1);

  LoopClosureRow lc2;
  lc2.closure_id = GenerateUuid();
  lc2.trajectory_id = traj_id;
  lc2.source_frame_id = GenerateUuid();
  lc2.target_frame_id = GenerateUuid();
  lc2.status = "rejected";
  lc2.inlier_ratio = 0.30;
  lc2.inlier_count = 20;
  lc2.confidence = 0.25;
  lc2.created_at_ns = 2000;
  db_.AddLoopClosure(lc2);

  const auto all = db_.FindLoopClosuresByTrajectory(traj_id);
  EXPECT_EQ(all.size(), 2u);

  const auto accepted = db_.FindAcceptedLoopClosuresByTrajectory(traj_id);
  EXPECT_EQ(accepted.size(), 1u);
  EXPECT_EQ(accepted[0].status, "accepted");
  EXPECT_DOUBLE_EQ(accepted[0].inlier_ratio, 0.92);
  EXPECT_EQ(accepted[0].inlier_count, 150);
}

// --- OptimizationResult CRUD ---

TEST_F(TrajectoryDbTest, InsertAndQueryOptimizationResult) {
  const auto traj_id = GenerateUuid();
  const auto graph_id = GenerateUuid();

  OptimizationResultRow res;
  res.result_id = GenerateUuid();
  res.graph_id = graph_id;
  res.trajectory_id = traj_id;
  res.status = "converged";
  res.iterations = 50;
  res.initial_error = 1000.0;
  res.final_error = 10.0;
  res.error_reduction = 0.99;
  res.created_at_ns = 1000;
  res.document_json = R"({"result_id":"r1"})";
  db_.AddOptimizationResult(res);

  const auto latest =
      db_.QueryLatestOptimizationResultByTrajectory(traj_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->result_id, res.result_id);
  EXPECT_EQ(latest->status, "converged");
  EXPECT_EQ(latest->iterations, 50);
  EXPECT_DOUBLE_EQ(latest->initial_error, 1000.0);
  EXPECT_DOUBLE_EQ(latest->final_error, 10.0);
  EXPECT_DOUBLE_EQ(latest->error_reduction, 0.99);
  EXPECT_EQ(latest->document_json, R"({"result_id":"r1"})");
}

TEST_F(TrajectoryDbTest, FindOptimizationResultsByTrajectory) {
  const auto traj_id = GenerateUuid();

  OptimizationResultRow r1;
  r1.result_id = GenerateUuid();
  r1.graph_id = GenerateUuid();
  r1.trajectory_id = traj_id;
  r1.status = "failed";
  r1.iterations = 100;
  r1.created_at_ns = 1000;
  db_.AddOptimizationResult(r1);

  OptimizationResultRow r2;
  r2.result_id = GenerateUuid();
  r2.graph_id = GenerateUuid();
  r2.trajectory_id = traj_id;
  r2.status = "converged";
  r2.iterations = 50;
  r2.created_at_ns = 2000;
  db_.AddOptimizationResult(r2);

  const auto all = db_.FindOptimizationResultsByTrajectory(traj_id);
  EXPECT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].status, "failed");
  EXPECT_EQ(all[1].status, "converged");
}

// --- Read-only tests ---

TEST_F(TrajectoryDbTest, TrajectoryInsertReadOnlyThrows) {
  db_.Close();
  auto ro = MetadataDb::OpenReadOnly(path_);

  TrajectoryRow row;
  row.trajectory_id = GenerateUuid();
  row.scene_id = scene_.scene_id;
  row.session_id = GenerateUuid();
  row.kind = "slam";
  row.status = "building";
  row.created_at_ns = 1000;
  EXPECT_THROW(ro.AddTrajectory(row), StorageError);
}

TEST_F(TrajectoryDbTest, PoseGraphInsertReadOnlyThrows) {
  db_.Close();
  auto ro = MetadataDb::OpenReadOnly(path_);

  PoseGraphRow row;
  row.graph_id = GenerateUuid();
  row.trajectory_id = GenerateUuid();
  row.scene_id = scene_.scene_id;
  row.status = "building";
  row.created_at_ns = 1000;
  EXPECT_THROW(ro.AddPoseGraph(row), StorageError);
}

TEST_F(TrajectoryDbTest, LoopClosureInsertReadOnlyThrows) {
  db_.Close();
  auto ro = MetadataDb::OpenReadOnly(path_);

  LoopClosureRow row;
  row.closure_id = GenerateUuid();
  row.trajectory_id = GenerateUuid();
  row.source_frame_id = GenerateUuid();
  row.target_frame_id = GenerateUuid();
  row.status = "accepted";
  row.created_at_ns = 1000;
  EXPECT_THROW(ro.AddLoopClosure(row), StorageError);
}

TEST_F(TrajectoryDbTest, OptimizationResultInsertReadOnlyThrows) {
  db_.Close();
  auto ro = MetadataDb::OpenReadOnly(path_);

  OptimizationResultRow row;
  row.result_id = GenerateUuid();
  row.graph_id = GenerateUuid();
  row.trajectory_id = GenerateUuid();
  row.status = "converged";
  row.created_at_ns = 1000;
  EXPECT_THROW(ro.AddOptimizationResult(row), StorageError);
}

// --- Full pipeline test ---

TEST_F(TrajectoryDbTest, FullPipelineRoundTrip) {
  const auto session_id = GenerateUuid();

  // 1. Create trajectory
  TrajectoryRow traj;
  traj.trajectory_id = GenerateUuid();
  traj.scene_id = scene_.scene_id;
  traj.session_id = session_id;
  traj.kind = "slam";
  traj.coordinate_frame = "trajectory_0";
  traj.status = "building";
  traj.node_count = 100;
  traj.total_distance_m = 50.5;
  traj.total_duration_ns = 30000000000LL;
  traj.created_at_ns = 1000;
  traj.document_json = R"({"schema_version":1})";
  db_.AddTrajectory(traj);

  // 2. Create pose graph
  PoseGraphRow pg;
  pg.graph_id = GenerateUuid();
  pg.trajectory_id = traj.trajectory_id;
  pg.scene_id = scene_.scene_id;
  pg.status = "ready";
  pg.node_count = 100;
  pg.edge_count = 150;
  pg.odometry_edge_count = 99;
  pg.loop_closure_edge_count = 40;
  pg.prior_edge_count = 11;
  pg.created_at_ns = 2000;
  db_.AddPoseGraph(pg);

  // 3. Add loop closure candidates
  LoopClosureCandidateRow cand;
  cand.candidate_id = GenerateUuid();
  cand.trajectory_id = traj.trajectory_id;
  cand.source_frame_id = GenerateUuid();
  cand.target_frame_id = GenerateUuid();
  cand.feature_match_score = 0.85;
  cand.matcher = "bow";
  cand.created_at_ns = 3000;
  db_.AddLoopClosureCandidate(cand);

  // 4. Add loop closures
  LoopClosureRow lc;
  lc.closure_id = GenerateUuid();
  lc.trajectory_id = traj.trajectory_id;
  lc.candidate_id = cand.candidate_id;
  lc.source_frame_id = cand.source_frame_id;
  lc.target_frame_id = cand.target_frame_id;
  lc.status = "accepted";
  lc.inlier_ratio = 0.92;
  lc.inlier_count = 150;
  lc.confidence = 0.95;
  lc.temporal_separation_ns = 5000000000LL;
  lc.spatial_separation_m = 25.3;
  lc.created_at_ns = 4000;
  db_.AddLoopClosure(lc);

  // 5. Run optimization
  OptimizationResultRow opt;
  opt.result_id = GenerateUuid();
  opt.graph_id = pg.graph_id;
  opt.trajectory_id = traj.trajectory_id;
  opt.status = "converged";
  opt.iterations = 50;
  opt.initial_error = 1000.0;
  opt.final_error = 10.0;
  opt.error_reduction = 0.99;
  opt.created_at_ns = 5000;
  opt.document_json = R"({"schema_version":1})";
  db_.AddOptimizationResult(opt);

  // Verify everything
  const auto latest_traj = db_.QueryLatestTrajectoryBySession(session_id);
  ASSERT_TRUE(latest_traj.has_value());
  EXPECT_EQ(latest_traj->trajectory_id, traj.trajectory_id);

  const auto latest_pg =
      db_.QueryLatestPoseGraphByTrajectory(traj.trajectory_id);
  ASSERT_TRUE(latest_pg.has_value());
  EXPECT_EQ(latest_pg->graph_id, pg.graph_id);

  const auto candidates =
      db_.FindLoopClosureCandidatesByTrajectory(traj.trajectory_id);
  EXPECT_EQ(candidates.size(), 1u);

  const auto closures =
      db_.FindLoopClosuresByTrajectory(traj.trajectory_id);
  EXPECT_EQ(closures.size(), 1u);

  const auto accepted =
      db_.FindAcceptedLoopClosuresByTrajectory(traj.trajectory_id);
  EXPECT_EQ(accepted.size(), 1u);

  const auto opt_results =
      db_.FindOptimizationResultsByTrajectory(traj.trajectory_id);
  EXPECT_EQ(opt_results.size(), 1u);
  EXPECT_EQ(opt_results[0].status, "converged");
}

}  // namespace
}  // namespace spatial::core
