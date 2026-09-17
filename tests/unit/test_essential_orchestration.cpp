// P3.1 Step 3: orchestrator wiring tests (S3-1–S3-4).
//
// Proves the VerifyLoopClosureGeometry unit->metric chain end to end WITHOUT
// touching the runner, CLI, PoseGraph assembly, GTSAM, or the frozen D6
// resolver: calibrated scene -> FeatureArtifacts (CAS) -> Essential provider
// -> frozen ResolveMetricTranslationFromUnitDirection -> metric LoopClosure.
// S3-5 (no GTSAM) holds statically: this file and its target link no GTSAM.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "adapters/visual_geometry/essential_verifier.h"
#include "adapters/visual_geometry/fundamental_verifier.h"
#include "adapters/visual_matching/visual_matcher_adapter.h"
#include "core/artifacts/artifact_store.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/loop_closure/geometric_verifier.h"
#include "core/loop_closure/verification_options.h"
#include "core/reconstruction/reconstruction.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/pose_graph_helpers.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/feature_extraction.h"
#include "engine/pipeline/loop_closure_to_pose_graph.h"
#include "engine/pipeline/loop_closure_verification.h"

namespace spatial::engine {
namespace {

using spatial::adapters::visual_geometry::EssentialGeometricVerifier;
using spatial::adapters::visual_geometry::FundamentalGeometricVerifier;
using spatial::adapters::visual_matching::L2NearestMatcher;
using spatial::core::ArtifactStore;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::GeometricVerificationOptions;
using spatial::core::LoopClosure;
using spatial::core::LoopClosureCandidate;
using spatial::core::MetadataDb;
using spatial::core::MetricBasis;
using spatial::core::MetricBasisSource;
using spatial::core::MetricBasisType;
using spatial::core::MetricLoopClosureMeasurement;
using spatial::core::ReconCamera;
using spatial::core::TrajectoryPoseNode;
using spatial::core::UnitRelativePose;
using spatial::core::Uuid;
using nlohmann::json;

constexpr double kFx = 800.0;
constexpr double kFy = 800.0;
constexpr double kCx = 320.0;
constexpr double kCy = 240.0;
constexpr double kPi = 3.14159265358979323846;

ReconCamera TestCamera() {
  ReconCamera cam;
  cam.camera_id = 0;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsic_model = "pinhole";
  cam.fx = kFx;
  cam.fy = kFy;
  cam.cx = kCx;
  cam.cy = kCy;
  cam.distortion_model = "none";
  return cam;
}

Eigen::Matrix3d GroundTruthRotation() {
  const Eigen::AngleAxisd yaw(12.0 * kPi / 180.0, Eigen::Vector3d::UnitY());
  const Eigen::AngleAxisd pitch(-8.0 * kPi / 180.0, Eigen::Vector3d::UnitX());
  const Eigen::AngleAxisd roll(5.0 * kPi / 180.0, Eigen::Vector3d::UnitZ());
  return (yaw * pitch * roll).toRotationMatrix();
}

Eigen::Vector3d GroundTruthTranslation() { return {0.6, -0.2, 0.15}; }

// The fixture renders X_t = R_gt * X_s + t_gt (epipolar convention) with the
// source camera at the world origin / identity rotation. The target camera's
// canonical world-from-camera pose is therefore R_wt = R_gt^T and its centre
// (= the pose translation, §4.8) is c_t = -R_gt^T * t_gt. The canonical
// T_source_target the frozen resolver must recover is exactly (R_wt, c_t).
Eigen::Matrix3d TrueTargetRotation() {
  return GroundTruthRotation().transpose();
}

Eigen::Vector3d TrueTargetTranslation() {
  return -GroundTruthRotation().transpose() * GroundTruthTranslation();
}

// Drift orthogonal to the measured direction, so the frozen resolver's
// projection recovers the TRUE baseline exactly (its documented blind spot is
// scale drift ALONG the direction, which this fixture deliberately excludes).
Eigen::Vector3d OrthogonalDrift(double magnitude) {
  const Eigen::Vector3d dir = TrueTargetTranslation().normalized();
  const Eigen::Vector3d v(0.1, 0.5, 0.2);
  const Eigen::Vector3d perp = v - v.dot(dir) * dir;
  return perp.normalized() * magnitude;
}

std::array<double, 4> QuatXyzw(const Eigen::Matrix3d& R) {
  const Eigen::Quaterniond q(R);
  return {q.x(), q.y(), q.z(), q.w()};
}

class EssentialOrchestrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_ess_orch_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = MetadataDb::Create(root_ / "project.db");
    db_->InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978",
                       "local", "{}", "{}");
    store_ = std::make_unique<ArtifactStore>(root_ / "artifacts", *db_);
  }

  void TearDown() override {
    store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Renders an exact calibrated pair under (R_gt, t_gt) and stores both
  // FeatureArtifacts; returns frame inputs WITH calibration attached.
  struct CalibratedFrames {
    VerificationFrameInput source;
    VerificationFrameInput target;
  };

  CalibratedFrames StoreCalibratedScene(const Eigen::Matrix3d& R_gt,
                                        const Eigen::Vector3d& t_gt,
                                        int n_points, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> ux(-2.0, 2.0);
    std::uniform_real_distribution<double> uy(-1.5, 1.5);
    std::uniform_real_distribution<double> uz(3.0, 8.0);
    std::vector<std::pair<double, double>> src_px, tgt_px;
    while (static_cast<int>(src_px.size()) < n_points) {
      const Eigen::Vector3d X(ux(rng), uy(rng), uz(rng));
      const Eigen::Vector3d Xt = R_gt * X + t_gt;
      if (Xt.z() < 0.5) continue;
      const double sx = kFx * X.x() / X.z() + kCx;
      const double sy = kFy * X.y() / X.z() + kCy;
      const double tx = kFx * Xt.x() / Xt.z() + kCx;
      const double ty = kFy * Xt.y() / Xt.z() + kCy;
      if (sx < 8.0 || sx > 632.0 || sy < 8.0 || sy > 472.0) continue;
      if (tx < 8.0 || tx > 632.0 || ty < 8.0 || ty > 472.0) continue;
      src_px.push_back({sx, sy});
      tgt_px.push_back({tx, ty});
    }
    const Uuid f0 = GenerateUuid(), f1 = GenerateUuid();
    CalibratedFrames frames;
    frames.source = StoreFeature(f0, 2000, src_px);
    frames.target = StoreFeature(f1, 1000, tgt_px);
    frames.source.camera = TestCamera();
    frames.target.camera = TestCamera();
    src_id_ = FormatUuid(f0);
    tgt_id_ = FormatUuid(f1);
    return frames;
  }

  VerificationFrameInput StoreFeature(
      const Uuid& frame_id, std::int64_t ts,
      const std::vector<std::pair<double, double>>& pts) {
    WriteFeatureArtifactInput input;
    input.frame_id = frame_id;
    input.detector = "mock";
    input.descriptor_type = "mock_16";
    input.input_content_hash = "image-" + FormatUuid(frame_id);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      input.keypoints.push_back({pts[i].first, pts[i].second, 1.0, 0.0, 0.5});
      std::uint32_t h = static_cast<std::uint32_t>(i + 1) * 2654435761u;
      std::vector<double> row(16);
      for (auto& v : row) {
        h = h * 1664525u + 1013904223u;
        v = static_cast<double>(h >> 8) * (1.0 / 16777216.0);
      }
      input.descriptors.push_back(std::move(row));
    }
    const auto res = WriteFeatureArtifactPayload(*store_, input);
    VerificationFrameInput f;
    f.frame_id = frame_id;
    f.timestamp_ns = ts;
    f.feature_artifact_uuid = res.artifact_uuid;
    return f;
  }

  LoopClosureCandidate Candidate(const std::string& traj) {
    LoopClosureCandidate c;
    c.trajectory_id = traj;
    c.source_frame_id = src_id_;
    c.target_frame_id = tgt_id_;
    c.feature_match_score = 999.0;  // never drives acceptance
    c.matcher = "visual_l2_nearest";
    c.candidate_id = FormatUuid(GenerateUuid());
    c.created_at_ns = 0;
    return c;
  }

  // GEOMETRICALLY CONSISTENT nodes: source at the origin with identity
  // rotation, target at its true world-from-camera pose plus an orthogonal
  // drift. The frozen resolver therefore recovers the TRUE baseline
  // lambda = |c_t| = |t_gt| = 0.65 m (cos_theta ~= 0.974 passes D-7c-9).
  std::vector<TrajectoryPoseNode> MetricNodes() {
    const Eigen::Vector3d drifted =
        TrueTargetTranslation() + OrthogonalDrift(0.15);
    const std::array<double, 4> rot = QuatXyzw(TrueTargetRotation());
    TrajectoryPoseNode src_node;
    src_node.frame_id = src_id_;
    src_node.timestamp_ns = 2000;
    src_node.position_xyz = {0.0, 0.0, 0.0};
    src_node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
    TrajectoryPoseNode tgt_node;
    tgt_node.frame_id = tgt_id_;
    tgt_node.timestamp_ns = 1000;
    tgt_node.position_xyz = {drifted.x(), drifted.y(), drifted.z()};
    tgt_node.rotation_xyzw = rot;
    return {src_node, tgt_node};
  }

  MetricBasis ValidBasis() {
    MetricBasis b;
    b.declared = true;
    b.source = MetricBasisSource::kTrajectory;
    b.basis = MetricBasisType::kOdometryScale;
    b.scale_calibration_ref = "calib-ref-1";
    b.provenance.configuration_hash = "cfg-hash";
    return b;
  }

  static const Uuid kProjectId;
  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> store_;
  std::string src_id_;
  std::string tgt_id_;
};

const Uuid EssentialOrchestrationTest::kProjectId = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};

// S3-1 — calibrated pose WITHOUT metric basis: unit valid, resolver gated,
// closure stays verified-visual-only, frozen stage emits no edge.
TEST_F(EssentialOrchestrationTest, S3_1_NoBasisStaysVisualOnly) {
  CalibratedFrames frames = StoreCalibratedScene(
      GroundTruthRotation(), GroundTruthTranslation(), 48, 42u);
  const L2NearestMatcher matcher;
  const EssentialGeometricVerifier verifier;
  const std::string traj = FormatUuid(GenerateUuid());
  const auto cand = Candidate(traj);
  const auto nodes = MetricNodes();

  MetricResolutionInput metric;
  metric.trajectory_nodes = &nodes;
  metric.metric_basis = MetricBasis{};  // undeclared

  const auto res = VerifyLoopClosureGeometry(
      *store_, *db_, cand, frames.source, frames.target, matcher, verifier,
      GeometricVerificationOptions{}, "cfg-hash", metric);

  // Unit estimation succeeded...
  EXPECT_TRUE(res.geo.verified);
  EXPECT_EQ(res.geo.geometric_model, "essential");
  ASSERT_TRUE(res.geo.unit_relative_pose.has_value());
  EXPECT_EQ(res.closure.status, "accepted");
  // ...but without a declared basis nothing becomes metric: the verifier
  // evidence stays pristine AND the closure stays visual-only.
  EXPECT_FALSE(res.geo.has_relative_pose);
  EXPECT_FALSE(res.closure.has_relative_pose);
  EXPECT_EQ(res.closure.relative_position_xyz,
            (std::array<double, 3>{0.0, 0.0, 0.0}));

  // The frozen downstream stage agrees: ineligible -> no metric edge.
  LoopClosureToPoseGraphInput stage_in;
  stage_in.trajectory_nodes = &nodes;
  stage_in.closure = res.closure;
  stage_in.metric_basis = MetricBasis{};
  const auto stage = LoopClosureToPoseGraph(stage_in);
  EXPECT_FALSE(stage.metric_eligible);
  EXPECT_FALSE(stage.metric_edge.has_value());
}

// S3-2 — calibrated pose + valid metric basis: frozen resolver promotes to a
// REAL metric closure (metres, source frame), evidence stays unit-side.
TEST_F(EssentialOrchestrationTest, S3_2_ValidBasisPromotesToMetric) {
  CalibratedFrames frames = StoreCalibratedScene(
      GroundTruthRotation(), GroundTruthTranslation(), 48, 42u);
  const L2NearestMatcher matcher;
  const EssentialGeometricVerifier verifier;
  const std::string traj = FormatUuid(GenerateUuid());
  const auto cand = Candidate(traj);
  const auto nodes = MetricNodes();

  MetricResolutionInput metric;
  metric.trajectory_nodes = &nodes;
  metric.metric_basis = ValidBasis();

  const auto res = VerifyLoopClosureGeometry(
      *store_, *db_, cand, frames.source, frames.target, matcher, verifier,
      GeometricVerificationOptions{}, "cfg-hash", metric);

  ASSERT_TRUE(res.geo.verified);
  ASSERT_TRUE(res.geo.unit_relative_pose.has_value());
  // The verifier evidence is NOT rewritten by orchestration...
  EXPECT_FALSE(res.geo.has_relative_pose);
  // ...while the closure IS promoted by the frozen resolver.
  EXPECT_EQ(res.closure.status, "accepted");
  EXPECT_TRUE(res.closure.has_relative_pose);

  // METRES: the resolver recovers the TRUE baseline |c_t| = |t_gt| = 0.65 m
  // (the drift is orthogonal, so lambda is exact) although the provider only
  // ever saw a unit direction.
  const auto& pos = res.closure.relative_position_xyz;
  const double norm =
      std::sqrt(pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2]);
  EXPECT_NEAR(norm, GroundTruthTranslation().norm(), 1e-6);
  // SOURCE FRAME + canonical direction: t_st = R_ws^T (c_t - c_s) = c_t here.
  const Eigen::Vector3d t_hat = TrueTargetTranslation().normalized();
  const Eigen::Vector3d pos_v(pos[0], pos[1], pos[2]);
  EXPECT_GT(pos_v.normalized().dot(t_hat), 1.0 - 1e-9);
  // And it equals the true relative transform's translation, component-wise.
  const Eigen::Vector3d c_t = TrueTargetTranslation();
  EXPECT_NEAR(pos[0], c_t.x(), 1e-9);
  EXPECT_NEAR(pos[1], c_t.y(), 1e-9);
  EXPECT_NEAR(pos[2], c_t.z(), 1e-9);
  // Rotation passes through verbatim from the unit estimate.
  EXPECT_EQ(res.closure.relative_rotation_xyzw,
            res.geo.unit_relative_pose->rotation_xyzw);

  // The CAS payload carries the metric closure (schema-shaped, provenance kept).
  const auto bytes = store_->Get(res.payload_content_hash);
  ASSERT_TRUE(bytes.has_value());
  const auto payload =
      json::parse(std::string(bytes->begin(), bytes->end()));
  ASSERT_EQ(payload["closures"].size(), 1u);
  EXPECT_TRUE(payload["closures"][0]["has_relative_pose"].get<bool>());
  EXPECT_NEAR(
      payload["closures"][0]["relative_position_xyz"][0].get<double>(),
      c_t.x(), 1e-9);

  // And the frozen downstream stage now emits a REAL metric edge from it.
  LoopClosureToPoseGraphInput stage_in;
  stage_in.trajectory_nodes = &nodes;
  stage_in.closure = res.closure;
  stage_in.metric_basis = ValidBasis();
  const auto stage = LoopClosureToPoseGraph(stage_in);
  EXPECT_TRUE(stage.metric_eligible);
  ASSERT_TRUE(stage.metric_edge.has_value());
  EXPECT_EQ(stage.metric_edge->relative_position_xyz, pos);
}

// S3-3 — fundamental regression: same orchestration, uncalibrated verifier,
// behaviour unchanged (accepted visual-only, no unit pose, no metric).
TEST_F(EssentialOrchestrationTest, S3_3_FundamentalRegression) {
  CalibratedFrames frames = StoreCalibratedScene(
      GroundTruthRotation(), GroundTruthTranslation(), 48, 42u);
  const L2NearestMatcher matcher;
  const FundamentalGeometricVerifier verifier;
  const std::string traj = FormatUuid(GenerateUuid());
  const auto cand = Candidate(traj);
  const auto nodes = MetricNodes();

  MetricResolutionInput metric;
  metric.trajectory_nodes = &nodes;
  metric.metric_basis = ValidBasis();  // even WITH basis: no unit, no metric

  const auto res = VerifyLoopClosureGeometry(
      *store_, *db_, cand, frames.source, frames.target, matcher, verifier,
      GeometricVerificationOptions{}, "cfg-hash", metric);

  EXPECT_TRUE(res.geo.verified);
  EXPECT_EQ(res.geo.geometric_model, "fundamental");
  EXPECT_EQ(res.closure.status, "accepted");
  EXPECT_FALSE(res.geo.unit_relative_pose.has_value());
  EXPECT_FALSE(res.geo.has_relative_pose);
  EXPECT_FALSE(res.closure.has_relative_pose);
  EXPECT_EQ(res.closure.relative_position_xyz,
            (std::array<double, 3>{0.0, 0.0, 0.0}));
}

// S3-4 — the D6 resolver is USED unmodified: pin its frozen contract directly
// (pure arithmetic, no estimation noise): lambda=4 scales the unit vector.
TEST_F(EssentialOrchestrationTest, S3_4_FrozenResolverContract) {
  const std::string src = FormatUuid(GenerateUuid());
  const std::string tgt = FormatUuid(GenerateUuid());
  LoopClosure lc;
  lc.status = "accepted";
  lc.source_frame_id = src;
  lc.target_frame_id = tgt;
  // Same metric-consistent geometry as MetricNodes, with local frame ids. The
  // direction is the CANONICAL one (T_source_target translation direction),
  // which is what the corrected provider emits.
  const Eigen::Vector3d t_hat = TrueTargetTranslation().normalized();
  TrajectoryPoseNode src_node;
  src_node.frame_id = src;
  src_node.position_xyz = {0.0, 0.0, 0.0};
  src_node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  TrajectoryPoseNode tgt_node;
  tgt_node.frame_id = tgt;
  tgt_node.position_xyz = {4.0 * t_hat.x(), 4.0 * t_hat.y(),
                           4.0 * t_hat.z()};
  tgt_node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  const std::vector<TrajectoryPoseNode> local_nodes = {src_node, tgt_node};

  const std::array<double, 3> t_dir = {t_hat.x(), t_hat.y(), t_hat.z()};
  const std::array<double, 4> r_ident = {0.0, 0.0, 0.0, 1.0};

  const std::optional<MetricLoopClosureMeasurement> m =
      spatial::core::ResolveMetricTranslationFromUnitDirection(
          lc, local_nodes, t_dir, r_ident, 0.5);
  ASSERT_TRUE(m.has_value());
  EXPECT_NEAR(m->position_cs[0], 4.0 * t_hat.x(), 1e-12);
  EXPECT_NEAR(m->position_cs[1], 4.0 * t_hat.y(), 1e-12);
  EXPECT_NEAR(m->position_cs[2], 4.0 * t_hat.z(), 1e-12);
  EXPECT_EQ(m->rotation_cst, r_ident);
  EXPECT_DOUBLE_EQ(m->geometric_residual, 0.5);
}

}  // namespace
}  // namespace spatial::engine
