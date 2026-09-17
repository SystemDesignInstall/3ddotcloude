// P3.1 Step 4: runner context wiring tests (R1–R10).
//
// Drive the PRODUCTION path only: Engine::RunPipeline (reconstruction mode,
// no worker) with a run config carrying metric.trajectory + loop_closure.
// The fixture creates project state (reconstruction + feature artifacts in
// the CAS, trajectory JSON in the run config); the runner then reads it back
// through the production mechanisms (CAS reads, canonical reconstruction
// lookup, strict run-config parsing). No hardcoded cameras/nodes/basis in
// the runner; no resolver/basis invention. Links engine + core + the two
// classical visual adapters. No GTSAM.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "adapters/visual_geometry/essential_verifier.h"
#include "adapters/visual_geometry/fundamental_verifier.h"
#include "adapters/visual_matching/visual_matcher_adapter.h"
#include "core/artifacts/artifact_manifest.h"
#include "core/errors/project_error.h"
#include "core/geometry/reconstruction_optimizer.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/project/project.h"
#include "core/reconstruction/reconstruction.h"
#include "core/reconstruction/reconstruction_json.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/optimizer.h"
#include "core/trajectory/pose_graph_helpers.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/feature_extraction.h"
#include "engine/pipeline/loop_closure_to_pose_graph.h"
#include "engine/pipeline/production_pipelines.h"
#include "engine/pipeline/sparse_correction_runner.h"

namespace {

using spatial::adapters::visual_geometry::EssentialGeometricVerifier;
using spatial::adapters::visual_geometry::FundamentalGeometricVerifier;
using spatial::adapters::visual_matching::L2NearestMatcher;
using spatial::core::ArtifactManifest;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;
using spatial::core::ParseUuid;
using spatial::core::Project;
using spatial::core::ProjectInfo;
using spatial::core::ReconCamera;
using spatial::core::ReconImage;
using spatial::core::Reconstruction;
using spatial::core::ReconstructionToJson;
using spatial::core::Uuid;
using spatial::core::fs::Iso8601UtcNow;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::BundleAdjustmentTrace;
using spatial::core::geometry::ReconstructionOptimizer;
using spatial::engine::Engine;
using spatial::engine::kSparseCorrectionPipelineId;
using spatial::engine::LoopClosureToPoseGraphInput;
using spatial::engine::MakeSparseCorrectionRunner;
using spatial::engine::RegisterProductionPipelines;
using spatial::engine::SparseCorrectionProfile;
using spatial::engine::SparseCorrectionSeams;
using spatial::engine::WriteFeatureArtifactInput;
using spatial::engine::WriteFeatureArtifactPayload;
using nlohmann::json;

constexpr double kPi = 3.14159265358979323846;

Eigen::Matrix3d GroundTruthRotation() {
  const Eigen::AngleAxisd yaw(12.0 * kPi / 180.0, Eigen::Vector3d::UnitY());
  const Eigen::AngleAxisd pitch(-8.0 * kPi / 180.0, Eigen::Vector3d::UnitX());
  const Eigen::AngleAxisd roll(5.0 * kPi / 180.0, Eigen::Vector3d::UnitZ());
  return (yaw * pitch * roll).toRotationMatrix();
}

Eigen::Vector3d GroundTruthTranslation() { return {0.6, -0.2, 0.15}; }

// The scene renders X_t = R_gt * X_s + t_gt (epipolar convention) with the
// source camera at the world origin / identity rotation, so the target
// camera's canonical world-from-camera pose is R_wt = R_gt^T with centre
// c_t = -R_gt^T * t_gt. The canonical T_source_target the frozen resolver
// recovers is exactly (R_wt, c_t); |c_t| = |t_gt| = 0.65 m is the true
// baseline. Fixtures below place the trajectory nodes AND the reconstruction
// image poses at these values (plus an orthogonal drift) so the whole chain
// is one physically consistent scene.
Eigen::Matrix3d TrueTargetRotation() {
  return GroundTruthRotation().transpose();
}

Eigen::Vector3d TrueTargetTranslation() {
  return -GroundTruthRotation().transpose() * GroundTruthTranslation();
}

// Drift orthogonal to the measured direction: the frozen resolver's
// documented blind spot is scale drift ALONG the direction, which these
// fixtures deliberately exclude so lambda comes out exact.
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

// The drifted target pose shared by the trajectory nodes and the committed
// reconstruction image (they are the same physical camera state).
Eigen::Vector3d DriftedTargetTranslation() {
  return TrueTargetTranslation() + OrthogonalDrift(0.15);
}

// Deterministic stub seam (mirrors the P3.1 pipeline test stand-in).
class StubOptimizer : public ReconstructionOptimizer {
 public:
  BundleAdjustmentResult optimize(const BundleAdjustmentInput& input) override {
    BundleAdjustmentTrace trace;
    trace.converged = true;
    trace.iterations = 6;
    trace.rms_before_px = 2.0;
    trace.rms_after_px = 1.0;
    trace.mean_before_px = 1.9;
    trace.mean_after_px = 0.9;
    trace.inlier_count_before = 18;
    trace.outlier_count_before = 2;
    trace.inlier_count_after = 20;
    trace.outlier_count_after = 0;
    trace.threshold_px_before = 3.0;
    trace.threshold_px_after = 3.0;
    Reconstruction v = input.source;
    v.reconstruction_id = FormatUuid(GenerateUuid());
    v.status = "succeeded";
    v.created_at_ns = input.source.created_at_ns + 100;
    v.provenance.backend.name = "stub_optimizer";
    v.provenance.backend.version = "test";
    v.provenance.backend.adapter_version = "0.1.0";
    return {std::move(v), trace};
  }
};

struct RenderedView {
  std::vector<std::pair<double, double>> pixels;
};

struct RenderedPair {
  RenderedView src;
  RenderedView tgt;
};

// Renders an exact pair under (R_gt, t_gt) with PER-VIEW intrinsics.
RenderedPair RenderPair(const Eigen::Matrix3d& R_gt,
                        const Eigen::Vector3d& t_gt, double fx_s, double fx_t,
                        int n_points, std::uint32_t seed) {
  constexpr double kCx = 320.0, kCy = 240.0;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> ux(-2.0, 2.0);
  std::uniform_real_distribution<double> uy(-1.5, 1.5);
  std::uniform_real_distribution<double> uz(3.0, 8.0);
  RenderedPair pair;
  while (static_cast<int>(pair.src.pixels.size()) < n_points) {
    const Eigen::Vector3d X(ux(rng), uy(rng), uz(rng));
    const Eigen::Vector3d Xt = R_gt * X + t_gt;
    if (Xt.z() < 0.5) continue;
    const double sx = fx_s * X.x() / X.z() + kCx;
    const double sy = fx_s * X.y() / X.z() + kCy;
    const double tx = fx_t * Xt.x() / Xt.z() + kCx;
    const double ty = fx_t * Xt.y() / Xt.z() + kCy;
    if (sx < 8.0 || sx > 632.0 || sy < 8.0 || sy > 472.0) continue;
    if (tx < 8.0 || tx > 632.0 || ty < 8.0 || ty > 472.0) continue;
    pair.src.pixels.push_back({sx, sy});
    pair.tgt.pixels.push_back({tx, ty});
  }
  return pair;
}

ReconCamera PinholeCamera(std::uint32_t id, double fx) {
  ReconCamera cam;
  cam.camera_id = id;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsic_model = "pinhole";
  cam.fx = fx;
  cam.fy = fx;
  cam.cx = 320.0;
  cam.cy = 240.0;
  cam.distortion_model = "none";
  return cam;
}

// Pass-through converged stub: returns the initial nodes unchanged with a
// converged trace. Used ONLY to isolate pre-optimization (closure/graph)
// assertions in Step-4/5-era tests that predate the Step-6 optimizer
// requirement; the optimization outcome itself is never asserted from this
// stub (the O-tests cover the real backend and every failure mode).
class PassThroughTrajectoryOptimizer
    : public spatial::core::TrajectoryOptimizer {
 public:
  spatial::core::PoseOptimizationOutput optimize(
      const spatial::core::PoseOptimizationInput& input) override {
    spatial::core::PoseOptimizationOutput out;
    out.result_id = FormatUuid(GenerateUuid());
    out.optimized_nodes = input.initial_nodes;
    out.trace.converged = true;
    out.trace.status = "converged";
    out.trace.iterations = 1;
    out.trace.initial_error = 1.0;
    out.trace.final_error = 1.0;
    out.trace.error_reduction = 0.0;
    return out;
  }
};

class RunnerContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_p3_runner_ctx_" + std::to_string(std::time(nullptr)) +
             "_" + std::to_string(rand()));
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "p3-runner-context";
    info.created_at = Iso8601UtcNow();
    project_id_ = info.uuid;
    project_ = std::make_unique<Project>(
        Project::Create(root_ / "demo.spx", info));
  }

  void TearDown() override {
    project_.reset();
    engine_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::unique_ptr<Engine> MakeEngine(
      ReconstructionOptimizer* ba,
      spatial::core::LoopClosureFeatureMatcher* matcher,
      spatial::core::GeometricVerifier* verifier,
      spatial::core::TrajectoryOptimizer* trajectory_optimizer = nullptr) {
    auto& db = project_->db();
    auto& store = project_->artifacts();
    SparseCorrectionSeams seams;
    seams.ba_optimizer = ba;
    seams.matcher = matcher;
    seams.verifier = verifier;
    seams.trajectory_optimizer = trajectory_optimizer;
    auto engine = std::make_unique<Engine>(
        std::move(*project_),
        MakeSparseCorrectionRunner(db, store, seams, {}),
        SparseCorrectionProfile());
    project_.reset();
    RegisterProductionPipelines(engine->registry());
    return engine;
  }

  std::string PutReconstructionPayload(const std::string& document) {
    const std::vector<std::uint8_t> bytes(document.begin(), document.end());
    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = "reconstruction";
    manifest.schema_version = 2;
    manifest.producer = {"spatial-platform", "0.1.0", "test"};
    manifest.creation_timestamp = Iso8601UtcNow();
    manifest.file_size = static_cast<std::int64_t>(bytes.size());
    return project_->artifacts().Put(bytes, manifest).content_hash;
  }

  // Writes a FeatureArtifact and returns its uuid string.
  std::string PutFeatures(
      const Uuid& frame_id,
      const std::vector<std::pair<double, double>>& pts) {
    WriteFeatureArtifactInput input;
    input.frame_id = frame_id;
    input.detector = "mock";
    input.descriptor_type = "mock_16";
    input.input_content_hash = "image-" + FormatUuid(frame_id);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      input.keypoints.push_back(
          {pts[i].first, pts[i].second, 1.0, 0.0, 0.5});
      std::uint32_t h = static_cast<std::uint32_t>(i + 1) * 2654435761u;
      std::vector<double> row(16);
      for (auto& v : row) {
        h = h * 1664525u + 1013904223u;
        v = static_cast<double>(h >> 8) * (1.0 / 16777216.0);
      }
      input.descriptors.push_back(std::move(row));
    }
    return FormatUuid(
        WriteFeatureArtifactPayload(project_->artifacts(), input)
            .artifact_uuid);
  }

  // Two-camera / two-image canonical reconstruction; frame ids out.
  Reconstruction MakeTwoViewReconstruction(double fx_s, double fx_t,
                                           const std::string& src_frame,
                                           const std::string& tgt_frame) {
    Reconstruction rec;
    rec.reconstruction_id = FormatUuid(GenerateUuid());
    rec.scene_id = "22222222-2222-4222-8222-222222222222";
    rec.session_ids = {"33333333-3333-4333-8333-333333333333"};
    rec.coordinate_frame = "trajectory_0";  // same-frame declaration: the synthetic scene renders pixels and nodes in ONE frame (Step-7 CF-1)
    rec.status = "succeeded";
    rec.created_at_ns = 100;
    rec.provenance.backend.name = "colmap";
    rec.provenance.backend.version = "v2";
    rec.provenance.backend.adapter_version = "test";
    rec.provenance.configuration_hash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    rec.cameras.push_back(PinholeCamera(1, fx_s));
    rec.cameras.push_back(PinholeCamera(2, fx_t));
    ReconImage src_img;
    src_img.image_id = 1;
    src_img.camera_id = 1;
    src_img.frame_id = src_frame;
    src_img.name = "frame_s.jpg";
    // Canonical world-from-camera poses (§4.8) of the SAME physical scene the
    // pixels were rendered from: source at the origin, target at its drifted
    // pose (the drift is what the correction removes).
    src_img.pose.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
    src_img.pose.translation_xyz = {0.0, 0.0, 0.0};
    src_img.detected = true;
    const Eigen::Vector3d drifted = DriftedTargetTranslation();
    const std::array<double, 4> rot = QuatXyzw(TrueTargetRotation());
    ReconImage tgt_img;
    tgt_img.image_id = 2;
    tgt_img.camera_id = 2;
    tgt_img.frame_id = tgt_frame;
    tgt_img.name = "frame_t.jpg";
    tgt_img.pose.rotation_xyzw = {rot[0], rot[1], rot[2], rot[3]};
    tgt_img.pose.translation_xyz = {drifted.x(), drifted.y(), drifted.z()};
    tgt_img.detected = true;
    rec.images.push_back(src_img);
    rec.images.push_back(tgt_img);
    return rec;
  }

  json BasisJson(bool valid) {
    json b = {{"declared", true},
              {"type", "odometry_scale"},
              {"source", "trajectory"}};
    if (valid) {
      b["scale_calibration_ref"] = "calib-ref-1";
      b["provenance"] = {{"configuration_hash",
                          "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                          "bbbbbbbb"}};
    }
    return b;
  }

  json LcConfig(const std::string& traj, const std::string& src_frame,
                const std::string& tgt_frame, const std::string& src_art,
                const std::string& tgt_art, const json& nodes,
                const json& basis) {
    return {
        {"metric",
         {{"trajectory",
           {{"trajectory_id", traj},
            {"scene_id", "scene-1"},
            {"session_id", "session-1"},
            {"kind", "sfm"},
            {"coordinate_frame", "trajectory_0"},
            {"status", "building"},
            {"created_at_ns", 0},
            {"metric_basis", basis},
            {"nodes", nodes}}}}},
        {"loop_closure",
         {{"candidate",
           {{"candidate_id", FormatUuid(GenerateUuid())},
            {"trajectory_id", traj},
            {"source_frame_id", src_frame},
            {"target_frame_id", tgt_frame},
            {"feature_match_score", 999.0},
            {"matcher", "visual_l2_nearest"},
            {"created_at_ns", 0}}},
          {"source_frame",
           {{"frame_id", src_frame},
            {"timestamp_ns", 2000},
            {"feature_artifact_uuid", src_art}}},
          {"target_frame",
           {{"frame_id", tgt_frame},
            {"timestamp_ns", 1000},
            {"feature_artifact_uuid", tgt_art}}}}}};
  }

  json NodesJson(const std::string& src_frame, const std::string& tgt_frame) {
    const Eigen::Vector3d drifted = DriftedTargetTranslation();
    const std::array<double, 4> rot = QuatXyzw(TrueTargetRotation());
    return json::array(
        {{{"frame_id", src_frame},
          {"timestamp_ns", 2000},
          {"sequence_index", 0},
          {"position_xyz", {0.0, 0.0, 0.0}},
          {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}}},
         {{"frame_id", tgt_frame},
          {"timestamp_ns", 1000},
          {"sequence_index", 7},
          {"position_xyz", {drifted.x(), drifted.y(), drifted.z()}},
          {"rotation_xyzw", {rot[0], rot[1], rot[2], rot[3]}}}});
  }

  json RunConfig(const std::string& scene, const json& lc) {
    json cfg = {{"project_id", FormatUuid(project_id_)},
                {"scene_name", scene},
                {"random_seed", "pinned-step4"}};
    cfg.update(lc);
    return cfg;
  }

  // Reads the single loop-closure payload produced by a run.
  json LoopClosurePayload(Engine& engine) {
    const auto rows =
        engine.project().db().FindArtifactsByType("loop_closure");
    EXPECT_EQ(rows.size(), 1u);
    const auto bytes =
        engine.project().artifacts().Get(rows.front().content_hash);
    EXPECT_TRUE(bytes.has_value());
    return json::parse(std::string(bytes->begin(), bytes->end()));
  }

  void ExpectNothingCommitted(Engine& engine) {
    const auto scene =
        engine.project().db().FindSceneByProject(project_id_);
    if (scene.has_value()) {
      EXPECT_TRUE(engine.project()
                      .db()
                      .FindReconstructionsByScene(scene->scene_id)
                      .empty());
      EXPECT_TRUE(engine.project()
                      .db()
                      .FindArtifactsByType("loop_closure")
                      .empty());
    }
  }

  std::filesystem::path root_;
  std::unique_ptr<Project> project_;
  std::unique_ptr<Engine> engine_;
  Uuid project_id_{};
  StubOptimizer stub_;
  L2NearestMatcher matcher_;
  EssentialGeometricVerifier essential_;
  FundamentalGeometricVerifier fundamental_;
  PassThroughTrajectoryOptimizer passthrough_;
};

// R1 — per-frame camera resolution with DIFFERENT intrinsics per view
// (fx 800 vs 600): only the correct per-frame ReconCamera verifies.
TEST_F(RunnerContextTest, R1_PerFrameCameraResolution) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 600.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 600.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const Uuid f_src = spatial::core::ParseUuid(src_frame);
  const Uuid f_tgt = spatial::core::ParseUuid(tgt_frame);
  const std::string src_art = PutFeatures(f_src, pair.src.pixels);
  const std::string tgt_art = PutFeatures(f_tgt, pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           NodesJson(src_frame, tgt_frame),
                           BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_, &passthrough_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r1", lc).dump());

  ASSERT_EQ(manifest.status, "succeeded");
  const json payload = LoopClosurePayload(*engine_);
  ASSERT_EQ(payload["closures"].size(), 1u);
  EXPECT_EQ(payload["closures"][0]["status"].get<std::string>(), "accepted");
  EXPECT_TRUE(payload["closures"][0]["has_relative_pose"].get<bool>());
  const auto& pos = payload["closures"][0]["relative_position_xyz"];
  const double norm = std::sqrt(
      pos[0].get<double>() * pos[0].get<double>() +
      pos[1].get<double>() * pos[1].get<double>() +
      pos[2].get<double>() * pos[2].get<double>());
  EXPECT_NEAR(norm, GroundTruthTranslation().norm(), 1e-6);
}

// R2 — unknown source frame fails closed before any write.
TEST_F(RunnerContextTest, R2_MissingSourceCameraFailsClosed) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const std::string bogus = FormatUuid(GenerateUuid());  // not reconstructed
  const json lc = LcConfig(traj, bogus, tgt_frame, src_art, tgt_art,
                           NodesJson(bogus, tgt_frame), BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r2", lc).dump());

  EXPECT_EQ(manifest.status, "failed");
  ExpectNothingCommitted(*engine_);
}

// R3 — unknown target frame fails closed before any write.
TEST_F(RunnerContextTest, R3_MissingTargetCameraFailsClosed) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const std::string bogus = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, bogus, src_art, tgt_art,
                           NodesJson(src_frame, bogus), BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r3", lc).dump());

  EXPECT_EQ(manifest.status, "failed");
  ExpectNothingCommitted(*engine_);
}

// R4 — nodes that match no candidate frame: accepted visual-only, no metric.
TEST_F(RunnerContextTest, R4_UnmatchedNodesStayVisualOnly) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  // Well-formed nodes, but for unrelated frames: the resolver finds nothing.
  const json nodes = NodesJson(FormatUuid(GenerateUuid()),
                               FormatUuid(GenerateUuid()));
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           nodes, BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r4", lc).dump());

  ASSERT_EQ(manifest.status, "succeeded");
  const json payload = LoopClosurePayload(*engine_);
  ASSERT_EQ(payload["closures"].size(), 1u);
  EXPECT_EQ(payload["closures"][0]["status"].get<std::string>(), "accepted");
  EXPECT_FALSE(payload["closures"][0]["has_relative_pose"].get<bool>());
}

// R5a — half-configured segment (loop_closure without metric.trajectory)
// fails closed before any write.
TEST_F(RunnerContextTest, R5a_HalfConfiguredSegmentFailsClosed) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);
  const std::string traj = FormatUuid(GenerateUuid());

  json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                     NodesJson(src_frame, tgt_frame), BasisJson(true));
  lc.erase("metric");
  engine_ = MakeEngine(&stub_, &matcher_, &essential_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r5a", lc).dump());
  EXPECT_EQ(manifest.status, "failed");
  ExpectNothingCommitted(*engine_);
}

// R5b — trajectory with an empty nodes array fails closed (missing
// trajectory context), before any write.
TEST_F(RunnerContextTest, R5b_EmptyNodesFailClosed) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);
  const std::string traj = FormatUuid(GenerateUuid());

  json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                     json::array(), BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r5b", lc).dump());
  EXPECT_EQ(manifest.status, "failed");
  ExpectNothingCommitted(*engine_);
}

// R6 — valid metric basis reaches the frozen resolver (|t| = 7 m).
TEST_F(RunnerContextTest, R6_ValidBasisReachesResolver) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           NodesJson(src_frame, tgt_frame),
                           BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_, &passthrough_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r6", lc).dump());

  ASSERT_EQ(manifest.status, "succeeded");
  const json payload = LoopClosurePayload(*engine_);
  ASSERT_EQ(payload["closures"].size(), 1u);
  EXPECT_TRUE(payload["closures"][0]["has_relative_pose"].get<bool>());
  const auto& pos = payload["closures"][0]["relative_position_xyz"];
  const double norm = std::sqrt(
      pos[0].get<double>() * pos[0].get<double>() +
      pos[1].get<double>() * pos[1].get<double>() +
      pos[2].get<double>() * pos[2].get<double>());
  EXPECT_NEAR(norm, GroundTruthTranslation().norm(), 1e-6);
}

// R7 — by-fiat basis (declared=true, no evidence): visual-only, no edge.
TEST_F(RunnerContextTest, R7_ByFiatBasisYieldsNoMetricEdge) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           NodesJson(src_frame, tgt_frame),
                           BasisJson(false));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r7", lc).dump());

  // declared=true alone is not an error — but produces no metric closure.
  ASSERT_EQ(manifest.status, "succeeded");
  const json payload = LoopClosurePayload(*engine_);
  ASSERT_EQ(payload["closures"].size(), 1u);
  EXPECT_EQ(payload["closures"][0]["status"].get<std::string>(), "accepted");
  EXPECT_FALSE(payload["closures"][0]["has_relative_pose"].get<bool>());

  // The frozen downstream stage emits no edge from it (INV-3).
  spatial::core::LoopClosure closure;
  closure.status = "accepted";
  closure.source_frame_id = src_frame;
  closure.target_frame_id = tgt_frame;
  closure.has_relative_pose = false;
  const Eigen::Vector3d drifted = DriftedTargetTranslation();
  const std::array<double, 4> rot = QuatXyzw(TrueTargetRotation());
  std::vector<spatial::core::TrajectoryPoseNode> nodes;
  spatial::core::TrajectoryPoseNode src_node;
  src_node.frame_id = src_frame;
  src_node.position_xyz = {0.0, 0.0, 0.0};
  src_node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  spatial::core::TrajectoryPoseNode tgt_node;
  tgt_node.frame_id = tgt_frame;
  tgt_node.position_xyz = {drifted.x(), drifted.y(), drifted.z()};
  tgt_node.rotation_xyzw = rot;
  nodes.push_back(src_node);
  nodes.push_back(tgt_node);
  spatial::core::MetricBasis by_fiat;
  by_fiat.declared = true;  // no evidence -> ineligible
  LoopClosureToPoseGraphInput stage_in;
  stage_in.trajectory_nodes = &nodes;
  stage_in.closure = closure;
  stage_in.metric_basis = by_fiat;
  const auto stage =
      spatial::engine::LoopClosureToPoseGraph(stage_in);
  EXPECT_FALSE(stage.metric_edge.has_value());
}

// R8 — fundamental path through the runner is unchanged (visual-only).
TEST_F(RunnerContextTest, R8_FundamentalPathUnchanged) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           NodesJson(src_frame, tgt_frame),
                           BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &fundamental_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r8", lc).dump());

  ASSERT_EQ(manifest.status, "succeeded");
  const json payload = LoopClosurePayload(*engine_);
  ASSERT_EQ(payload["closures"].size(), 1u);
  EXPECT_EQ(payload["closures"][0]["status"].get<std::string>(), "accepted");
  EXPECT_FALSE(payload["closures"][0]["has_relative_pose"].get<bool>());
  EXPECT_GT(payload["closures"][0]["inlier_count"].get<std::int64_t>(), 0);
}

// R9 — the runner-produced metric closure feeds the frozen stage as metres:
// edge translation has |t| = lambda (7 m), never the unit direction.
TEST_F(RunnerContextTest, R9_UnitNeverReachesGraphAsMeters) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           NodesJson(src_frame, tgt_frame),
                           BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_, &passthrough_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step4_r9", lc).dump());
  ASSERT_EQ(manifest.status, "succeeded");

  // Rebuild the closure exactly as persisted and run the frozen stage.
  const json payload = LoopClosurePayload(*engine_);
  const auto& c = payload["closures"][0];
  spatial::core::LoopClosure closure;
  closure.status = c["status"].get<std::string>();
  closure.source_frame_id = src_frame;
  closure.target_frame_id = tgt_frame;
  closure.has_relative_pose = c["has_relative_pose"].get<bool>();
  ASSERT_TRUE(closure.has_relative_pose);
  for (int i = 0; i < 3; ++i)
    closure.relative_position_xyz[static_cast<std::size_t>(i)] =
        c["relative_position_xyz"][i].get<double>();
  for (int i = 0; i < 4; ++i)
    closure.relative_rotation_xyzw[static_cast<std::size_t>(i)] =
        c["relative_rotation_xyzw"][i].get<double>();

  const Eigen::Vector3d drifted = DriftedTargetTranslation();
  const std::array<double, 4> rot = QuatXyzw(TrueTargetRotation());
  std::vector<spatial::core::TrajectoryPoseNode> nodes;
  spatial::core::TrajectoryPoseNode src_node;
  src_node.frame_id = src_frame;
  src_node.position_xyz = {0.0, 0.0, 0.0};
  src_node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  spatial::core::TrajectoryPoseNode tgt_node;
  tgt_node.frame_id = tgt_frame;
  tgt_node.position_xyz = {drifted.x(), drifted.y(), drifted.z()};
  tgt_node.rotation_xyzw = rot;
  nodes.push_back(src_node);
  nodes.push_back(tgt_node);
  spatial::core::MetricBasis basis;
  basis.declared = true;
  basis.scale_calibration_ref = "calib-ref-1";
  basis.provenance.configuration_hash =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  LoopClosureToPoseGraphInput stage_in;
  stage_in.trajectory_nodes = &nodes;
  stage_in.closure = closure;
  stage_in.metric_basis = basis;
  const auto stage =
      spatial::engine::LoopClosureToPoseGraph(stage_in);
  ASSERT_TRUE(stage.metric_edge.has_value());
  const auto& edge_pos = stage.metric_edge->relative_position_xyz;
  const double norm = std::sqrt(edge_pos[0] * edge_pos[0] +
                                edge_pos[1] * edge_pos[1] +
                                edge_pos[2] * edge_pos[2]);
  EXPECT_NEAR(norm, GroundTruthTranslation().norm(), 1e-6);  // metres (lambda), never |t| = 1
  EXPECT_EQ(edge_pos, closure.relative_position_xyz);  // verbatim, unscaled
}

// R10 — deterministic resolution: two IDENTICAL independent fixtures (same
// ground-truth values, distinct UUIDs) resolve to the same metric values.
//
// NOTE (pre-existing, out of scope): re-running the SAME pipeline twice on
// one project fails at the commit — CommitWorkerReconstructionArtifact
// re-inserts the same reconstruction_id PRIMARY KEY (AC-22 non-idempotent
// re-run, frozen orchestrator). R10 therefore proves determinism across
// identical inputs, not in-place re-run persistence.
TEST_F(RunnerContextTest, R10_DeterministicResolution) {
  // Twin A.
  const std::string src_a = FormatUuid(GenerateUuid());
  const std::string tgt_a = FormatUuid(GenerateUuid());
  const RenderedPair pair_a = RenderPair(GroundTruthRotation(),
                                         GroundTruthTranslation(), 800.0,
                                         800.0, 48, 42u);
  const Reconstruction recon_a =
      MakeTwoViewReconstruction(800.0, 800.0, src_a, tgt_a);
  const std::string hash_a =
      PutReconstructionPayload(ReconstructionToJson(recon_a));
  const std::string src_art_a =
      PutFeatures(spatial::core::ParseUuid(src_a), pair_a.src.pixels);
  const std::string tgt_art_a =
      PutFeatures(spatial::core::ParseUuid(tgt_a), pair_a.tgt.pixels);
  const std::string traj_a = FormatUuid(GenerateUuid());
  const json lc_a = LcConfig(traj_a, src_a, tgt_a, src_art_a, tgt_art_a,
                             NodesJson(src_a, tgt_a), BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_, &passthrough_);
  const auto first = engine_->RunPipeline(kSparseCorrectionPipelineId,
                                          {hash_a},
                                          RunConfig("step4_r10a", lc_a).dump());
  ASSERT_EQ(first.status, "succeeded");
  const json payload_first = LoopClosurePayload(*engine_);
  engine_.reset();

  // Twin B is rebuilt from scratch in a fresh project below. Because each
  // TEST_F gets a fresh fixture, twin B is constructed by re-running the
  // same deterministic builders with fresh UUIDs: same pixels, same nodes
  // values, same basis values. The assertion compares metric VALUES.
  const std::string src_b = FormatUuid(GenerateUuid());
  const std::string tgt_b = FormatUuid(GenerateUuid());
  // NOTE: RenderPair is seeded identically, so pixels are identical values.
  const RenderedPair pair_b = RenderPair(GroundTruthRotation(),
                                         GroundTruthTranslation(), 800.0,
                                         800.0, 48, 42u);
  // Re-create the project state (SetUp ran once; emulate a fresh twin by
  // building a second independent project directory inline).
  const std::filesystem::path root_b =
      std::filesystem::path(root_.string() + "_twin");
  std::error_code ec_b;
  std::filesystem::remove_all(root_b, ec_b);
  ProjectInfo info_b;
  info_b.uuid = GenerateUuid();
  info_b.name = "p3-r10-twin";
  info_b.created_at = Iso8601UtcNow();
  Project project_b = Project::Create(root_b / "demo.spx", info_b);
  const Reconstruction recon_b =
      MakeTwoViewReconstruction(800.0, 800.0, src_b, tgt_b);
  const std::string doc_b = ReconstructionToJson(recon_b);
  const std::vector<std::uint8_t> bytes_b(doc_b.begin(), doc_b.end());
  ArtifactManifest manifest_b;
  manifest_b.artifact_uuid = GenerateUuid();
  manifest_b.type = "reconstruction";
  manifest_b.schema_version = 2;
  manifest_b.producer = {"spatial-platform", "0.1.0", "test"};
  manifest_b.creation_timestamp = Iso8601UtcNow();
  manifest_b.file_size = static_cast<std::int64_t>(bytes_b.size());
  const std::string hash_b =
      project_b.artifacts().Put(bytes_b, manifest_b).content_hash;

  auto put_features_b = [&](const Uuid& frame_id,
                            const std::vector<std::pair<double, double>>& pts) {
    WriteFeatureArtifactInput input;
    input.frame_id = frame_id;
    input.detector = "mock";
    input.descriptor_type = "mock_16";
    input.input_content_hash = "image-" + FormatUuid(frame_id);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      input.keypoints.push_back(
          {pts[i].first, pts[i].second, 1.0, 0.0, 0.5});
      std::uint32_t h = static_cast<std::uint32_t>(i + 1) * 2654435761u;
      std::vector<double> row(16);
      for (auto& v : row) {
        h = h * 1664525u + 1013904223u;
        v = static_cast<double>(h >> 8) * (1.0 / 16777216.0);
      }
      input.descriptors.push_back(std::move(row));
    }
    return FormatUuid(
        WriteFeatureArtifactPayload(project_b.artifacts(), input)
            .artifact_uuid);
  };
  const std::string src_art_b =
      put_features_b(spatial::core::ParseUuid(src_b), pair_b.src.pixels);
  const std::string tgt_art_b =
      put_features_b(spatial::core::ParseUuid(tgt_b), pair_b.tgt.pixels);
  const std::string traj_b = FormatUuid(GenerateUuid());
  const json lc_b = LcConfig(traj_b, src_b, tgt_b, src_art_b, tgt_art_b,
                             NodesJson(src_b, tgt_b), BasisJson(true));
  json cfg_b = {{"project_id", FormatUuid(info_b.uuid)},
                {"scene_name", "step4_r10b"},
                {"random_seed", "pinned-step4"}};
  cfg_b.update(lc_b);
  auto& db_b = project_b.db();
  auto& store_b = project_b.artifacts();
  SparseCorrectionSeams seams_b;
  seams_b.ba_optimizer = &stub_;
  seams_b.matcher = &matcher_;
  seams_b.verifier = &essential_;
  PassThroughTrajectoryOptimizer passthrough_b;
  seams_b.trajectory_optimizer = &passthrough_b;
  Engine engine_b(std::move(project_b),
                  MakeSparseCorrectionRunner(db_b, store_b, seams_b, {}),
                  SparseCorrectionProfile());
  RegisterProductionPipelines(engine_b.registry());
  const auto second = engine_b.RunPipeline(kSparseCorrectionPipelineId,
                                           {hash_b}, cfg_b.dump());
  ASSERT_EQ(second.status, "succeeded");
  const auto rows_b =
      engine_b.project().db().FindArtifactsByType("loop_closure");
  ASSERT_EQ(rows_b.size(), 1u);
  const auto bytes_second_b =
      engine_b.project().artifacts().Get(rows_b.front().content_hash);
  ASSERT_TRUE(bytes_second_b.has_value());
  const json payload_second = json::parse(
      std::string(bytes_second_b->begin(), bytes_second_b->end()));

  // Same verification outcome and bit-identical metric values across the
  // identical inputs (deterministic camera/trajectory resolution).
  EXPECT_EQ(payload_first["closures"][0]["status"].get<std::string>(),
            payload_second["closures"][0]["status"].get<std::string>());
  EXPECT_EQ(payload_first["closures"][0]["has_relative_pose"].get<bool>(),
            payload_second["closures"][0]["has_relative_pose"].get<bool>());
  for (int i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(
        payload_first["closures"][0]["relative_position_xyz"][i]
            .get<double>(),
        payload_second["closures"][0]["relative_position_xyz"][i]
            .get<double>());
    EXPECT_DOUBLE_EQ(
        payload_first["closures"][0]["relative_rotation_xyzw"][i]
            .get<double>(),
        payload_second["closures"][0]["relative_rotation_xyzw"][i]
            .get<double>());
  }
  EXPECT_DOUBLE_EQ(
      payload_first["closures"][0]["relative_rotation_xyzw"][3].get<double>(),
      payload_second["closures"][0]["relative_rotation_xyzw"][3]
          .get<double>());
  std::filesystem::remove_all(root_b, ec_b);
}

// G1 — basis absent (by-fiat): verified unit, visual-only closure, and NO
// PoseGraph evidence anywhere (INV-3 holds through the production runner).
TEST_F(RunnerContextTest, G1_NoBasisNoGraph) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           NodesJson(src_frame, tgt_frame),
                           BasisJson(false));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step5_g1", lc).dump());

  ASSERT_EQ(manifest.status, "succeeded");
  // Closure verified but visual-only (R7); graph layer persists nothing.
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindPoseGraphsByTrajectory(ParseUuid(traj))
                  .empty());
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindArtifactsByType("pose_graph")
                  .empty());
}

// G2 — valid basis: candidate -> calibrated verification -> metric
// resolution -> LoopClosure -> PoseGraph edge, through RunPipeline only.
// The edge translation is the metric translation VERBATIM (|t| == lambda,
// no rescaling), and node ids resolve back to the candidate frames.
TEST_F(RunnerContextTest, G2_MetricClosureBecomesGraphEdge) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);

  const std::string traj = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           NodesJson(src_frame, tgt_frame),
                           BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_, &passthrough_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step5_g2", lc).dump());
  ASSERT_EQ(manifest.status, "succeeded");

  // One queryable graph row, attached to the run's scene.
  const auto graphs =
      engine_->project().db().FindPoseGraphsByTrajectory(ParseUuid(traj));
  ASSERT_EQ(graphs.size(), 1u);
  EXPECT_EQ(graphs[0].status, "ready");
  EXPECT_EQ(graphs[0].node_count, 2);
  EXPECT_EQ(graphs[0].edge_count, 1);
  EXPECT_EQ(graphs[0].loop_closure_edge_count, 1);
  EXPECT_EQ(graphs[0].odometry_edge_count, 0);
  const auto scene =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  EXPECT_EQ(graphs[0].scene_id, scene->scene_id);

  // The row document is the canonical graph: edge == metric translation.
  const json doc = json::parse(graphs[0].document_json);
  ASSERT_EQ(doc["nodes"].size(), 2u);
  ASSERT_EQ(doc["edges"].size(), 1u);
  const auto& edge = doc["edges"][0];
  EXPECT_EQ(edge["type"].get<std::string>(), "loop_closure");
  // Frame -> node correspondence (the Step-4 caveat, proven end to end):
  // node ids resolve back to the candidate's frames.
  EXPECT_EQ(doc["nodes"][edge["source_node_id"].get<int>()]["frame_id"]
                .get<std::string>(),
            src_frame);
  EXPECT_EQ(doc["nodes"][edge["target_node_id"].get<int>()]["frame_id"]
                .get<std::string>(),
            tgt_frame);
  const auto& epos = edge["relative_position_xyz"];
  const double enorm =
      std::sqrt(epos[0].get<double>() * epos[0].get<double>() +
                epos[1].get<double>() * epos[1].get<double>() +
                epos[2].get<double>() * epos[2].get<double>());
  // |edge| == the true baseline recovered by the frozen resolver (metres),
  // never the unit direction and never rescaled by the graph stage.
  EXPECT_NEAR(enorm, GroundTruthTranslation().norm(), 1e-6);
  // Verbatim against the loop-closure evidence payload (no second converter).
  const json lc_payload = LoopClosurePayload(*engine_);
  const auto& cpos =
      lc_payload["closures"][0]["relative_position_xyz"];
  for (int i = 0; i < 3; ++i)
    EXPECT_DOUBLE_EQ(epos[i].get<double>(), cpos[i].get<double>());
  // The frozen D3 information matrix is valid SPD.
  std::array<double, 36> info{};
  for (int i = 0; i < 36; ++i)
    info[static_cast<std::size_t>(i)] =
        edge["information_matrix_6x6"][i].get<double>();
  EXPECT_TRUE(spatial::core::ValidateInformationMatrix(info).ok);

  // One canonical CAS payload with evidence lineage to the loop closure.
  const auto graph_artifacts =
      engine_->project().db().FindArtifactsByType("pose_graph");
  ASSERT_EQ(graph_artifacts.size(), 1u);
  const auto graph_bytes = engine_->project().artifacts().Get(
      graph_artifacts.front().content_hash);
  ASSERT_TRUE(graph_bytes.has_value());
  const json graph_payload = json::parse(
      std::string(graph_bytes->begin(), graph_bytes->end()));
  ASSERT_EQ(graph_payload["edges"].size(), 1u);
  EXPECT_DOUBLE_EQ(
      graph_payload["edges"][0]["relative_position_xyz"][0].get<double>(),
      epos[0].get<double>());
  const auto lc_rows =
      engine_->project().db().FindArtifactsByType("loop_closure");
  ASSERT_EQ(lc_rows.size(), 1u);
  const auto graph_manifest = engine_->project().artifacts().ReadManifest(
      graph_artifacts.front().artifact_id);
  ASSERT_TRUE(graph_manifest.has_value());
  ASSERT_EQ(graph_manifest->input_artifact_hashes.size(), 1u);
  EXPECT_EQ(graph_manifest->input_artifact_hashes.front(),
            lc_rows.front().content_hash);
}

// G3 — no LC segment requested: the plain commit + BA path persists no
// graph evidence at all.
TEST_F(RunnerContextTest, G3_NoSegmentNoGraph) {
  const Reconstruction recon = MakeTwoViewReconstruction(
      800.0, 800.0, FormatUuid(GenerateUuid()), FormatUuid(GenerateUuid()));
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  engine_ = MakeEngine(&stub_, &matcher_, &essential_);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step5_g3", json::object()).dump());

  ASSERT_EQ(manifest.status, "succeeded");
  const auto scene =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindPoseGraphsByScene(scene->scene_id)
                  .empty());
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindArtifactsByType("pose_graph")
                  .empty());
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindArtifactsByType("loop_closure")
                  .empty());
}

// Configurable seam stub for the optimizer failure modes (O4-O6): the
// production-call shape is identical; only the backend outcome varies.
class FailingTrajectoryOptimizer
    : public spatial::core::TrajectoryOptimizer {
 public:
  enum class Mode { kThrow, kFailed, kNaN };
  explicit FailingTrajectoryOptimizer(Mode mode) : mode_(mode) {}
  spatial::core::PoseOptimizationOutput optimize(
      const spatial::core::PoseOptimizationInput& input) override {
    if (mode_ == Mode::kThrow) {
      throw spatial::core::ProjectError(spatial::core::ErrorCode::kInternal,
                                        "stub optimizer: injected failure");
    }
    spatial::core::PoseOptimizationOutput out;
    out.result_id = FormatUuid(GenerateUuid());
    out.optimized_nodes = input.initial_nodes;
    if (mode_ == Mode::kNaN && out.optimized_nodes.size() > 1) {
      out.optimized_nodes[1].position_xyz = {
          std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    }
    if (mode_ == Mode::kFailed) {
      out.trace.converged = false;
      out.trace.status = "failed";
    } else {
      out.trace.converged = true;
      out.trace.status = "converged";
    }
    out.trace.iterations = 2;
    out.trace.initial_error = 2.0;
    out.trace.final_error = 1.0;
    out.trace.error_reduction = 0.5;
    return out;
  }

 private:
  Mode mode_;
};

// Shared Step-6 setup: the standard valid-basis λ=7 scene through RunPipeline
// with the given optimizer seam. Returns the manifest status + trajectory id.
struct OptimizerCaseResult {
  std::string status;
  std::string traj;
};

class OptimizerCaseRunner : public RunnerContextTest {
 protected:
  OptimizerCaseResult RunOptimizerCase(
      const std::string& scene,
      spatial::core::TrajectoryOptimizer* optimizer) {
    const std::string src_frame = FormatUuid(GenerateUuid());
    const std::string tgt_frame = FormatUuid(GenerateUuid());
    const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                         GroundTruthTranslation(), 800.0,
                                         800.0, 48, 42u);
    const Reconstruction recon =
        MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
    const std::string recon_hash =
        PutReconstructionPayload(ReconstructionToJson(recon));
    const std::string src_art =
        PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
    const std::string tgt_art =
        PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);
    const std::string traj = FormatUuid(GenerateUuid());
    const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                             NodesJson(src_frame, tgt_frame),
                             BasisJson(true));
    engine_ = MakeEngine(&stub_, &matcher_, &essential_, optimizer);
    const auto manifest = engine_->RunPipeline(
        kSparseCorrectionPipelineId, {recon_hash},
        RunConfig(scene, lc).dump());
    return {manifest.status, traj};
  }

  // Post-commit failure shape (O3-O6): evidence persists (commit + closure +
  // graph), the correction never completes, and NO optimization result exists.
  // NOTE (pre-existing scheduler behaviour, not Step-6 scope): a failed stage
  // is retried while the error is recoverable, so verification evidence may
  // accumulate across attempts (commit PK keeps revisions at exactly one).
  void ExpectFailedOptimization(const std::string& traj) {
    const auto scene =
        engine_->project().db().FindSceneByProject(project_id_);
    ASSERT_TRUE(scene.has_value());
    const auto recons = engine_->project().db().FindReconstructionsByScene(
        scene->scene_id);
    ASSERT_EQ(recons.size(), 1u);  // committed v2 only: BA never ran
    EXPECT_EQ(recons[0].status, "succeeded");
    const auto closures = engine_->project().db().FindLoopClosuresByTrajectory(
        ParseUuid(traj));
    EXPECT_GE(closures.size(), 1u);
    for (const auto& row : closures) EXPECT_EQ(row.status, "accepted");
    EXPECT_EQ(engine_->project()
                  .db()
                  .FindPoseGraphsByTrajectory(ParseUuid(traj))
                  .size(),
              1u);
    EXPECT_TRUE(engine_->project()
                    .db()
                    .FindOptimizationResultsByTrajectory(ParseUuid(traj))
                    .empty());
    EXPECT_TRUE(engine_->project()
                    .db()
                    .FindArtifactsByType("optimization_result")
                    .empty());
  }
};

// O3 — graph assembled but no optimizer seam: the stage fails (no silent
// skip of a "ready" graph), evidence persists, no optimization result.
TEST_F(OptimizerCaseRunner, O3_MissingOptimizerSeamFailsClosed) {
  const auto result = RunOptimizerCase("step6_o3", nullptr);
  EXPECT_EQ(result.status, "failed");
  ExpectFailedOptimization(result.traj);
}

// O4 — optimizer backend throws: fail closed, no optimization result.
TEST_F(OptimizerCaseRunner, O4_ThrowingOptimizerFailsClosed) {
  FailingTrajectoryOptimizer failing(
      FailingTrajectoryOptimizer::Mode::kThrow);
  const auto result = RunOptimizerCase("step6_o4", &failing);
  EXPECT_EQ(result.status, "failed");
  ExpectFailedOptimization(result.traj);
}

// O5 — optimizer reports non-converged: fail closed, initial nodes are NOT
// consumed as an optimized solution.
TEST_F(OptimizerCaseRunner, O5_NonConvergedOptimizerFailsClosed) {
  FailingTrajectoryOptimizer failing(
      FailingTrajectoryOptimizer::Mode::kFailed);
  const auto result = RunOptimizerCase("step6_o5", &failing);
  EXPECT_EQ(result.status, "failed");
  ExpectFailedOptimization(result.traj);
}

// O6 — optimizer returns non-finite poses: fail closed, no false trajectory.
TEST_F(OptimizerCaseRunner, O6_NonFiniteOptimizerFailsClosed) {
  FailingTrajectoryOptimizer failing(FailingTrajectoryOptimizer::Mode::kNaN);
  const auto result = RunOptimizerCase("step6_o6", &failing);
  EXPECT_EQ(result.status, "failed");
  ExpectFailedOptimization(result.traj);
}

// O7 — no graph assembled (fundamental path): the optimizer seam is never
// required; the run succeeds with no optimization evidence.
TEST_F(OptimizerCaseRunner, O7_NoGraphSkipsOptimizer) {
  const std::string src_frame = FormatUuid(GenerateUuid());
  const std::string tgt_frame = FormatUuid(GenerateUuid());
  const RenderedPair pair = RenderPair(GroundTruthRotation(),
                                       GroundTruthTranslation(), 800.0, 800.0,
                                       48, 42u);
  const Reconstruction recon =
      MakeTwoViewReconstruction(800.0, 800.0, src_frame, tgt_frame);
  const std::string recon_hash =
      PutReconstructionPayload(ReconstructionToJson(recon));
  const std::string src_art =
      PutFeatures(spatial::core::ParseUuid(src_frame), pair.src.pixels);
  const std::string tgt_art =
      PutFeatures(spatial::core::ParseUuid(tgt_frame), pair.tgt.pixels);
  const std::string traj = FormatUuid(GenerateUuid());
  const json lc = LcConfig(traj, src_frame, tgt_frame, src_art, tgt_art,
                           NodesJson(src_frame, tgt_frame),
                           BasisJson(true));
  engine_ = MakeEngine(&stub_, &matcher_, &fundamental_, nullptr);
  const auto manifest = engine_->RunPipeline(
      kSparseCorrectionPipelineId, {recon_hash},
      RunConfig("step6_o7", lc).dump());

  EXPECT_EQ(manifest.status, "succeeded");
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindPoseGraphsByTrajectory(ParseUuid(traj))
                  .empty());
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindOptimizationResultsByTrajectory(ParseUuid(traj))
                  .empty());
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindArtifactsByType("optimization_result")
                  .empty());
}

}  // namespace
