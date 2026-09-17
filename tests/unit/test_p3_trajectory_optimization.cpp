// P3.1 Step 6 + Step 8 + Step 9: PoseGraph -> GTSAM TrajectoryOptimizer plus
// the retriangulation (v3) -> bundle adjustment (v4) production tests.
//
// O1 proves REAL optimization through the production path only
// (Engine::RunPipeline, reconstruction mode, no worker): a drifted trajectory
// node is pulled back by the loop-closure edge, with strictly decreasing
// GTSAM-computed graph errors. O2 proves measurement immutability: the loop
// closure, graph edge, and initial nodes are never mutated; the optimized
// trajectory is a NEW result.
//
// The RT* (Step 8 retriangulation via the CommitGeometryStage seam,
// spatial_retriangulator) and BT* (Step 9 bundle adjustment via the
// ReconstructionOptimizer seam) tests run the FULL corrected chain
// v0(committed) -> v1(applied) -> v2(committed copy) -> v3(retriangulated)
// -> v4(BA refined) through Engine::RunPipeline; the RT family drives the
// deterministic StubOptimizer (pure chain semantics) while the BT family
// drives the REAL GtsamBundleAdjustmentOptimizer through the same seam.
//
// GTSAM/Boost registration constraints mirror test_gtsam_adapter.cpp:
// GTest first, own main(), manual add_test (no discovery).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "adapters/gtsam/gtsam_bundle_adjustment_optimizer.h"
#include "adapters/gtsam/gtsam_optimizer_adapter.h"
#include "adapters/visual_geometry/essential_verifier.h"
#include "adapters/visual_geometry/fundamental_verifier.h"
#include "adapters/visual_matching/visual_matcher_adapter.h"
#include "core/artifacts/artifact_manifest.h"
#include "core/geometry/reprojection.h"
#include "core/geometry/se3.h"
#include "core/geometry/triangulation.h"
#include "core/project/project.h"
#include "core/reconstruction/reconstruction.h"
#include "core/reconstruction/reconstruction_json.h"
#include "core/storage/metadata_db.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/feature_extraction.h"
#include "engine/pipeline/production_pipelines.h"
#include "engine/pipeline/sparse_correction_runner.h"

namespace {

using spatial::adapters::gtsam::GtsamBundleAdjustmentOptimizer;
using spatial::adapters::gtsam::GtsamTrajectoryOptimizer;
using spatial::adapters::visual_geometry::EssentialGeometricVerifier;
using spatial::adapters::visual_geometry::FundamentalGeometricVerifier;
using spatial::adapters::visual_matching::L2NearestMatcher;
using spatial::core::ArtifactManifest;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::ParseUuid;
using spatial::core::Project;
using spatial::core::ProjectInfo;
using spatial::core::ReconCamera;
using spatial::core::ReconImage;
using spatial::core::ReconPoint3D;
using spatial::core::Reconstruction;
using spatial::core::ReconstructionToJson;
using spatial::core::Uuid;
using spatial::core::fs::Iso8601UtcNow;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::BundleAdjustmentTrace;
using spatial::core::geometry::CameraModel;
using spatial::core::geometry::CameraView;
using spatial::core::geometry::Quaternion;
using spatial::core::geometry::ReconstructionOptimizer;
using spatial::core::geometry::SE3;
using spatial::core::geometry::TriangulateTwoRays;
using spatial::core::geometry::TriangulationAcceptance;
using spatial::core::geometry::TriangulationCandidate;
using spatial::engine::Engine;
using spatial::engine::FeatureExtractionResult;
using spatial::engine::kSparseCorrectionPipelineId;
using spatial::engine::MakeSparseCorrectionRunner;
using spatial::engine::RegisterProductionPipelines;
using spatial::engine::SparseCorrectionProfile;
using spatial::engine::SparseCorrectionSeams;
using spatial::engine::WriteFeatureArtifact;
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
// camera's canonical world-from-camera pose (§4.8) is R_wt = R_gt^T with
// centre c_t = -R_gt^T * t_gt. |c_t| = |t_gt| = 0.65 m is the TRUE baseline
// the frozen resolver must recover. The committed reconstruction and the
// trajectory nodes both carry this pose PLUS a drift (position orthogonal to
// the measured direction, so lambda stays exact; plus a small rotation error),
// which is what the correction removes.
Eigen::Matrix3d TrueTargetRotation() {
  return GroundTruthRotation().transpose();
}

Eigen::Vector3d TrueTargetTranslation() {
  return -GroundTruthRotation().transpose() * GroundTruthTranslation();
}

Eigen::Vector3d OrthogonalDrift(double magnitude) {
  const Eigen::Vector3d dir = TrueTargetTranslation().normalized();
  const Eigen::Vector3d v(0.1, 0.5, 0.2);
  const Eigen::Vector3d perp = v - v.dot(dir) * dir;
  return perp.normalized() * magnitude;
}

Eigen::Vector3d DriftedTargetTranslation() {
  return TrueTargetTranslation() + OrthogonalDrift(0.15);
}

Eigen::Matrix3d DriftedTargetRotation() {
  const Eigen::AngleAxisd tilt(
      0.05, Eigen::Vector3d(0.3, 0.9, 0.3).normalized());
  return TrueTargetRotation() * tilt.toRotationMatrix();
}

std::array<double, 4> QuatXyzw(const Eigen::Matrix3d& R) {
  const Eigen::Quaterniond q(R);
  return {q.x(), q.y(), q.z(), q.w()};
}

class StubOptimizer : public ReconstructionOptimizer {
 public:
  explicit StubOptimizer(double rms_after = 1.0) : rms_after_(rms_after) {}
  BundleAdjustmentResult optimize(const BundleAdjustmentInput& input) override {
    BundleAdjustmentTrace trace;
    trace.converged = true;
    trace.iterations = 6;
    trace.rms_before_px = 2.0;
    trace.rms_after_px = rms_after_;
    trace.mean_before_px = 1.9;
    trace.mean_after_px = 0.9 * rms_after_;
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

  double rms_after_ = 1.0;
};

struct RenderedPair {
  std::vector<std::pair<double, double>> src;
  std::vector<std::pair<double, double>> tgt;
};

RenderedPair RenderPair(const Eigen::Matrix3d& R_gt,
                        const Eigen::Vector3d& t_gt, int n_points,
                        std::uint32_t seed) {
  constexpr double kFx = 800.0, kCx = 320.0, kCy = 240.0;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> ux(-2.0, 2.0);
  std::uniform_real_distribution<double> uy(-1.5, 1.5);
  std::uniform_real_distribution<double> uz(3.0, 8.0);
  RenderedPair pair;
  while (static_cast<int>(pair.src.size()) < n_points) {
    const Eigen::Vector3d X(ux(rng), uy(rng), uz(rng));
    const Eigen::Vector3d Xt = R_gt * X + t_gt;
    if (Xt.z() < 0.5) continue;
    const double sx = kFx * X.x() / X.z() + kCx;
    const double sy = kFx * X.y() / X.z() + kCy;
    const double tx = kFx * Xt.x() / Xt.z() + kCx;
    const double ty = kFx * Xt.y() / Xt.z() + kCy;
    if (sx < 8.0 || sx > 632.0 || sy < 8.0 || sy > 472.0) continue;
    if (tx < 8.0 || tx > 632.0 || ty < 8.0 || ty > 472.0) continue;
    pair.src.push_back({sx, sy});
    pair.tgt.push_back({tx, ty});
  }
  return pair;
}

// Step-8 fixture geometry: the SAME rendered scene as RenderPair, but the true
// 3D points are kept so the corrected (re-triangulated) geometry can be
// measured against ground truth. Pixels are exact (rendered from the true
// poses); the committed reconstruction only claims drifted poses, so its
// triangulated points come out wrong until Step 8 recomputes them.
struct RenderedScene {
  std::vector<Eigen::Vector3d> X;  // true world points (source camera at origin)
  RenderedPair pair;               // pixels in src / tgt, aligned with X
};

// Renders the fixed scene for the RETRIANGULATION/BA fixtures. `noise_px` (in
// pixels, standard deviation of independent zero-mean Gaussian perturbation per
// pixel) models real keypoint noise: the exact RT* fixtures use 0.0 (so the
// corrected geometry must land bit-on ground truth); the BA fixtures use a
// small non-zero sigma (so the retriangulated v3 keeps finite residuals for the
// D5 gate and the LM refinement has real signal to work from — with exact
// pixels + exact poses the v3 residual would already be zero and the gate would
// vacuously fail).
RenderedScene RenderScene(int n_points, std::uint32_t seed,
                          double noise_px = 0.0) {
  constexpr double kFx = 800.0, kCx = 320.0, kCy = 240.0;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> ux(-2.0, 2.0);
  std::uniform_real_distribution<double> uy(-1.5, 1.5);
  std::uniform_real_distribution<double> uz(3.0, 8.0);
  RenderedScene scene;
  while (static_cast<int>(scene.X.size()) < n_points) {
    const Eigen::Vector3d X(ux(rng), uy(rng), uz(rng));
    const Eigen::Vector3d Xt =
        GroundTruthRotation() * X + GroundTruthTranslation();
    if (Xt.z() < 0.5) continue;
    const double sx = kFx * X.x() / X.z() + kCx;
    const double sy = kFx * X.y() / X.z() + kCy;
    const double tx = kFx * Xt.x() / Xt.z() + kCx;
    const double ty = kFx * Xt.y() / Xt.z() + kCy;
    if (sx < 8.0 || sx > 632.0 || sy < 8.0 || sy > 472.0) continue;
    if (tx < 8.0 || tx > 632.0 || ty < 8.0 || ty > 472.0) continue;
    if (noise_px > 0.0) {
      std::normal_distribution<double> noise(0.0, noise_px);
      scene.pair.src.push_back({sx + noise(rng), sy + noise(rng)});
      scene.pair.tgt.push_back({tx + noise(rng), ty + noise(rng)});
    } else {
      scene.pair.src.push_back({sx, sy});
      scene.pair.tgt.push_back({tx, ty});
    }
    scene.X.push_back(X);
  }
  return scene;
}

ReconCamera PinholeCamera(std::uint32_t id) {
  ReconCamera cam;
  cam.camera_id = id;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsic_model = "pinhole";
  cam.fx = 800.0;
  cam.fy = 800.0;
  cam.cx = 320.0;
  cam.cy = 240.0;
  cam.distortion_model = "none";
  return cam;
}

// The committed 3D points as COLMAP would have produced them: DLT closest-point
// intersection of the two observation rays under the DRIFTED camera poses (the
// pose claim in the committed reconstruction document) using the exact scene
// pixels. The source camera is at the origin/identity, the target at its
// drifted pose. Points whose drifted-ray intersection fails the geometric
// predicates are skipped (they never enter the committed document) — each kept
// point's track references keypoints[src_idx]/keypoints[tgt_idx] verbatim.
struct CommittedGeometry {
  std::vector<ReconPoint3D> points;      // aligned one-to-one with truth
  std::vector<Eigen::Vector3d> truth;
};

CommittedGeometry CommittedPointsUnderDrift(const RenderedScene& scene) {
  const Eigen::Vector3d drifted_t = DriftedTargetTranslation();
  const std::array<double, 4> drifted_r = QuatXyzw(DriftedTargetRotation());
  CameraView src(1u, CameraModel::FromReconCamera(PinholeCamera(1)),
                 SE3(Quaternion(0.0, 0.0, 0.0, 1.0).Normalized(),
                     Eigen::Vector3d::Zero()));
  CameraView tgt(2u, CameraModel::FromReconCamera(PinholeCamera(2)),
                 SE3(Quaternion(drifted_r[0], drifted_r[1], drifted_r[2],
                                drifted_r[3])
                         .Normalized(),
                     drifted_t));
  CommittedGeometry out;
  for (std::size_t i = 0; i < scene.X.size(); ++i) {
    const Eigen::Vector2d p1(scene.pair.src[i].first,
                             scene.pair.src[i].second);
    const Eigen::Vector2d p2(scene.pair.tgt[i].first,
                             scene.pair.tgt[i].second);
    const TriangulationCandidate cand = TriangulateTwoRays(src, p1, tgt, p2);
    if (cand.acceptance != TriangulationAcceptance::kAccepted) continue;
    ReconPoint3D pt;
    pt.point3d_id = static_cast<std::uint64_t>(i + 1);
    const std::array<double, 3> xyz{cand.xyz[0], cand.xyz[1], cand.xyz[2]};
    pt.xyz = xyz;
    pt.track = {{1u, static_cast<std::int32_t>(i)},
                {2u, static_cast<std::int32_t>(i)}};
    out.points.push_back(std::move(pt));
    out.truth.push_back(scene.X[i]);
  }
  return out;
}

double RotationAngle(const Eigen::Matrix3d& A, const Eigen::Matrix3d& B) {
  const double trace = (A.transpose() * B).trace();
  const double clamped = std::max(-1.0, std::min(1.0, (trace - 1.0) * 0.5));
  return std::acos(clamped);
}

Eigen::Matrix3d QuatToMatrix(const json& q) {
  const Eigen::Quaterniond eq(q[3].get<double>(), q[0].get<double>(),
                              q[1].get<double>(), q[2].get<double>());
  return eq.normalized().toRotationMatrix();
}

class TrajectoryOptimizationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_p3_traj_opt_" + std::to_string(std::time(nullptr)) +
             "_" + std::to_string(rand()));
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "p3-traj-opt";
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

  // Runs the full production path with DRIFTED target node; returns the
  // manifest status and keeps engine_ for evidence queries. recon_frame sets
  // the reconstruction's declared frame (must name the trajectory frame for
  // Step-7 apply); optimizer overrides the GTSAM seam (failure-mode stubs).
  // `points` are the committed 3D points of the fixed scene (Step-8 fixture);
  // `rendered` supplies the exact rendered scene (same pixels) so tracks and
  // ground truth stay aligned; register_feature_sets=false writes payload-only
  // feature artifacts (no feature_sets row) for the RT5 fail-closed case.
  std::string RunDrifted(
      const std::string& scene, std::string* traj_out,
      const std::string& recon_frame = "trajectory_0",
      spatial::core::TrajectoryOptimizer* optimizer = nullptr,
      std::vector<ReconPoint3D> points = {},
      const RenderedScene* rendered = nullptr,
      bool register_feature_sets = true,
      spatial::core::geometry::ReconstructionOptimizer* ba_override = nullptr,
      bool metric_basis_declared = true,
      spatial::engine::ExecutionManifest* manifest_out = nullptr) {
    const std::string src_frame = FormatUuid(GenerateUuid());
    const std::string tgt_frame = FormatUuid(GenerateUuid());
    const RenderedPair pair =
        (rendered != nullptr)
            ? rendered->pair
            : RenderPair(GroundTruthRotation(), GroundTruthTranslation(), 48,
                         42u);
    Reconstruction recon;
    recon.reconstruction_id = FormatUuid(GenerateUuid());
    recon.scene_id = "22222222-2222-4222-8222-222222222222";
    recon.session_ids = {"33333333-3333-4333-8333-333333333333"};
    recon.coordinate_frame = recon_frame;
    recon.status = "succeeded";
    recon.created_at_ns = 100;
    recon.provenance.backend.name = "colmap";
    recon.provenance.backend.version = "v2";
    recon.provenance.backend.adapter_version = "test";
    recon.provenance.configuration_hash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    recon.cameras.push_back(PinholeCamera(1));
    recon.cameras.push_back(PinholeCamera(2));
    // Canonical world-from-camera poses of the SAME physical scene the pixels
    // were rendered from: source at the origin, target at its DRIFTED pose
    // (the drift is what the correction removes).
    const Eigen::Vector3d drifted_t = DriftedTargetTranslation();
    const std::array<double, 4> drifted_r = QuatXyzw(DriftedTargetRotation());
    ReconImage src_img;
    src_img.image_id = 1;
    src_img.camera_id = 1;
    src_img.frame_id = src_frame;
    src_img.name = "frame_s.jpg";
    src_img.pose.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
    src_img.pose.translation_xyz = {0.0, 0.0, 0.0};
    src_img.detected = true;
    ReconImage tgt_img;
    tgt_img.image_id = 2;
    tgt_img.camera_id = 2;
    tgt_img.frame_id = tgt_frame;
    tgt_img.name = "frame_t.jpg";
    tgt_img.pose.rotation_xyzw = drifted_r;
    tgt_img.pose.translation_xyz = {drifted_t.x(), drifted_t.y(),
                                    drifted_t.z()};
    tgt_img.detected = true;
    recon.images.push_back(src_img);
    recon.images.push_back(tgt_img);
    recon.points3D = std::move(points);
    const std::string doc = ReconstructionToJson(recon);
    const std::vector<std::uint8_t> doc_bytes(doc.begin(), doc.end());
    ArtifactManifest doc_manifest;
    doc_manifest.artifact_uuid = GenerateUuid();
    doc_manifest.type = "reconstruction";
    doc_manifest.schema_version = 2;
    doc_manifest.producer = {"spatial-platform", "0.1.0", "test"};
    doc_manifest.creation_timestamp = Iso8601UtcNow();
    doc_manifest.file_size = static_cast<std::int64_t>(doc_bytes.size());
    const std::string recon_hash =
        project_->artifacts().Put(doc_bytes, doc_manifest).content_hash;
    recon_hash_ = recon_hash;
    recon_doc_ = doc;

    auto put_features = [&](const Uuid& frame_id,
                            const std::vector<std::pair<double, double>>& pts,
                            std::string* content_hash) {
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
      FeatureExtractionResult result =
          register_feature_sets
              ? WriteFeatureArtifact(project_->artifacts(), project_->db(),
                                     input)
              : WriteFeatureArtifactPayload(project_->artifacts(), input);
      if (content_hash != nullptr) *content_hash = result.content_hash;
      return FormatUuid(result.artifact_uuid);
    };
    const std::string src_art =
        put_features(ParseUuid(src_frame), pair.src, &src_art_hash_);
    const std::string tgt_art =
        put_features(ParseUuid(tgt_frame), pair.tgt, &tgt_art_hash_);
    src_art_ = src_art;
    tgt_art_ = tgt_art;

    // Drifted trajectory context: the target node carries the SAME drifted
    // pose as the committed reconstruction image (one physical camera state).
    // The visual geometry (pixels) is exact, so any correction comes from REAL
    // optimization, not fixture noise.
    const Eigen::Vector3d drifted = DriftedTargetTranslation();
    const std::array<double, 4> drifted_node_r =
        QuatXyzw(DriftedTargetRotation());
    const std::string traj = FormatUuid(GenerateUuid());
    const json nodes = json::array(
        {{{"frame_id", src_frame},
          {"timestamp_ns", 2000},
          {"sequence_index", 0},
          {"position_xyz", {0.0, 0.0, 0.0}},
          {"rotation_xyzw", {0.0, 0.0, 0.0, 1.0}}},
         {{"frame_id", tgt_frame},
          {"timestamp_ns", 1000},
          {"sequence_index", 7},
          {"position_xyz", {drifted.x(), drifted.y(), drifted.z()}},
          {"rotation_xyzw",
           {drifted_node_r[0], drifted_node_r[1], drifted_node_r[2],
            drifted_node_r[3]}}}});
    const json basis = {{"declared", metric_basis_declared},
                        {"type", "odometry_scale"},
                        {"source", "trajectory"},
                        {"scale_calibration_ref", "calib-ref-1"},
                        {"provenance",
                         {{"configuration_hash",
                           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                           "bbbbbbbb"}}}};
    const json lc = {
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
    json cfg = {{"project_id", FormatUuid(project_id_)},
                {"scene_name", scene},
                {"random_seed", "pinned-step6"}};
    cfg.update(lc);

    auto& db = project_->db();
    auto& store = project_->artifacts();
    SparseCorrectionSeams seams;
    seams.ba_optimizer = (ba_override != nullptr) ? ba_override : &stub_;
    seams.matcher = &matcher_;
    seams.verifier = &essential_;
    seams.trajectory_optimizer = (optimizer != nullptr) ? optimizer : &gtsam_;
    engine_ = std::make_unique<Engine>(
        std::move(*project_),
        MakeSparseCorrectionRunner(db, store, seams, {}),
        SparseCorrectionProfile());
    project_.reset();
    RegisterProductionPipelines(engine_->registry());
    const auto manifest = engine_->RunPipeline(kSparseCorrectionPipelineId,
                                               {recon_hash}, cfg.dump());
    if (traj_out != nullptr) *traj_out = traj;
    if (manifest_out != nullptr) *manifest_out = manifest;
    src_frame_ = src_frame;
    tgt_frame_ = tgt_frame;
    return manifest.status;
  }

  json OptimizationPayload() {
    const auto rows =
        engine_->project().db().FindArtifactsByType("optimization_result");
    EXPECT_EQ(rows.size(), 1u);
    const auto bytes =
        engine_->project().artifacts().Get(rows.front().content_hash);
    EXPECT_TRUE(bytes.has_value());
    return json::parse(std::string(bytes->begin(), bytes->end()));
  }

  std::filesystem::path root_;
  std::unique_ptr<Project> project_;
  std::unique_ptr<Engine> engine_;
  Uuid project_id_{};
  StubOptimizer stub_;
  L2NearestMatcher matcher_;
  EssentialGeometricVerifier essential_;
  FundamentalGeometricVerifier fundamental_;
  GtsamTrajectoryOptimizer gtsam_;
  std::string src_frame_;
  std::string tgt_frame_;
  std::string recon_hash_;
  std::string recon_doc_;
  std::string src_art_;
  std::string tgt_art_;
  std::string src_art_hash_;
  std::string tgt_art_hash_;
};

// O1 — REAL improvement: GTSAM reduces the loop residual strictly and pulls
// the drifted node back onto the measured constraint. No hardcoded errors:
// every number below is computed by the backend at run time.
TEST_F(TrajectoryOptimizationTest, O1_RealImprovement) {
  std::string traj;
  ASSERT_EQ(RunDrifted("step6_o1", &traj), "succeeded");

  const auto rows = engine_->project().db().FindOptimizationResultsByTrajectory(
      ParseUuid(traj));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].status, "converged");
  EXPECT_GE(rows[0].iterations, 1);
  // There WAS error to remove, and strictly less remains afterwards.
  EXPECT_GT(rows[0].initial_error, 0.0);
  EXPECT_LT(rows[0].final_error, rows[0].initial_error);
  EXPECT_GT(rows[0].error_reduction, 0.5);
  EXPECT_LE(rows[0].error_reduction, 1.0);

  const json payload = OptimizationPayload();
  EXPECT_EQ(payload["status"].get<std::string>(), "converged");
  EXPECT_LT(payload["final_error"].get<double>(),
            payload["initial_error"].get<double>());
  ASSERT_EQ(payload["nodes"].size(), 2u);

  // The drifted target node is back on the measured constraint: with the
  // source anchored, the optimum IS the canonical measurement, i.e. the true
  // relative transform (R_wt, c_t) — the true 0.65 m baseline, not the drift.
  const Eigen::Vector3d drifted = DriftedTargetTranslation();
  const Eigen::Vector3d expect = TrueTargetTranslation();
  Eigen::Vector3d p_src(0, 0, 0), p_tgt(0, 0, 0);
  Eigen::Matrix3d r_tgt = Eigen::Matrix3d::Identity();
  for (const auto& n : payload["nodes"]) {
    const Eigen::Vector3d p(n["position_xyz"][0].get<double>(),
                            n["position_xyz"][1].get<double>(),
                            n["position_xyz"][2].get<double>());
    if (n["frame_id"].get<std::string>() == src_frame_) {
      p_src = p;
    } else {
      p_tgt = p;
      r_tgt = QuatToMatrix(n["rotation_xyzw"]);
    }
  }
  EXPECT_LT((p_tgt - expect).norm(), 1e-3);  // drift removed, really
  EXPECT_LT(p_src.norm(), 1e-3);             // anchor held node 0
  EXPECT_GT((drifted - expect).norm(), 0.1);  // (and there WAS drift)
  // Rotation improved toward the measured relative rotation as well.
  const double before = RotationAngle(DriftedTargetRotation(),
                                      TrueTargetRotation());
  EXPECT_GT(before, 0.01);
  EXPECT_LT(RotationAngle(r_tgt, TrueTargetRotation()), before);
  EXPECT_LT(RotationAngle(r_tgt, TrueTargetRotation()), 1e-2);
}

// O2 — measurement immutability: the closure, graph edge, and initial nodes
// are never mutated by optimization; the optimized trajectory is a NEW
// result referencing the same graph.
TEST_F(TrajectoryOptimizationTest, O2_MeasurementImmutable) {
  std::string traj;
  ASSERT_EQ(RunDrifted("step6_o2", &traj), "succeeded");

  // The loop-closure evidence is intact and metric.
  const auto lc_rows =
      engine_->project().db().FindArtifactsByType("loop_closure");
  ASSERT_EQ(lc_rows.size(), 1u);
  const auto lc_bytes =
      engine_->project().artifacts().Get(lc_rows.front().content_hash);
  ASSERT_TRUE(lc_bytes.has_value());
  const json lc_payload =
      json::parse(std::string(lc_bytes->begin(), lc_bytes->end()));
  ASSERT_TRUE(
      lc_payload["closures"][0]["has_relative_pose"].get<bool>());

  // Exactly one graph, whose edge is still the verbatim metric measurement.
  const auto graphs =
      engine_->project().db().FindPoseGraphsByTrajectory(ParseUuid(traj));
  ASSERT_EQ(graphs.size(), 1u);
  const json graph_doc = json::parse(graphs[0].document_json);
  ASSERT_EQ(graph_doc["edges"].size(), 1u);
  for (int i = 0; i < 3; ++i)
    EXPECT_DOUBLE_EQ(
        graph_doc["edges"][0]["relative_position_xyz"][i].get<double>(),
        lc_payload["closures"][0]["relative_position_xyz"][i].get<double>());

  // Exactly one NEW optimization result, referencing the same graph.
  const auto opt_rows =
      engine_->project().db().FindOptimizationResultsByTrajectory(
          ParseUuid(traj));
  ASSERT_EQ(opt_rows.size(), 1u);
  const json payload = OptimizationPayload();
  EXPECT_EQ(payload["graph_id"].get<std::string>(),
            graph_doc["graph_id"].get<std::string>());
  EXPECT_EQ(payload["trajectory_id"].get<std::string>(), traj);
  // ...and its nodes really moved (a new solution, not an echo of input).
  const Eigen::Vector3d drifted = DriftedTargetTranslation();
  for (const auto& n : payload["nodes"]) {
    if (n["frame_id"].get<std::string>() == tgt_frame_) {
      const Eigen::Vector3d p(n["position_xyz"][0].get<double>(),
                              n["position_xyz"][1].get<double>(),
                              n["position_xyz"][2].get<double>());
      EXPECT_GT((p - drifted).norm(), 0.1);
    }
  }
}

// Converged stub whose target node references an unknown frame (dangling
// measurement for the apply boundary; sizes still match).
class DanglingNodeOptimizer : public spatial::core::TrajectoryOptimizer {
 public:
  spatial::core::PoseOptimizationOutput optimize(
      const spatial::core::PoseOptimizationInput& input) override {
    spatial::core::PoseOptimizationOutput out;
    out.result_id = FormatUuid(GenerateUuid());
    out.optimized_nodes = input.initial_nodes;
    if (out.optimized_nodes.size() > 1) {
      out.optimized_nodes[1].frame_id =
          FormatUuid(GenerateUuid());  // nowhere in the reconstruction
    }
    out.trace.converged = true;
    out.trace.status = "converged";
    out.trace.iterations = 2;
    out.trace.initial_error = 2.0;
    out.trace.final_error = 1.0;
    out.trace.error_reduction = 0.5;
    return out;
  }
};

// Converged stub with no nodes at all (vacuous apply).
class EmptyNodesOptimizer : public spatial::core::TrajectoryOptimizer {
 public:
  spatial::core::PoseOptimizationOutput optimize(
      const spatial::core::PoseOptimizationInput& input) override {
    (void)input;
    spatial::core::PoseOptimizationOutput out;
    out.result_id = FormatUuid(GenerateUuid());
    out.trace.converged = true;
    out.trace.status = "converged";
    out.trace.iterations = 1;
    out.trace.initial_error = 0.0;
    out.trace.final_error = 0.0;
    out.trace.error_reduction = 0.0;
    return out;
  }
};

class ApplyOptimizedTrajectoryTest : public TrajectoryOptimizationTest {
 protected:
  // Finds the applied revision document (backend spatial_optimizer), if any.
  bool FindApplied(json* doc_out) {
    const auto scene =
        engine_->project().db().FindSceneByProject(project_id_);
    if (!scene.has_value()) return false;
    const auto rows = engine_->project().db().FindReconstructionsByScene(
        scene->scene_id);
    for (const auto& row : rows) {
      const json doc = json::parse(row.document_json);
      if (doc["provenance"]["backend"]["name"].get<std::string>() ==
          "spatial_optimizer") {
        if (doc_out != nullptr) *doc_out = doc;
        return true;
      }
    }
    return false;
  }

  json LoopClosurePayload() {
    const auto rows =
        engine_->project().db().FindArtifactsByType("loop_closure");
    EXPECT_EQ(rows.size(), 1u);
    const auto bytes =
        engine_->project().artifacts().Get(rows.front().content_hash);
    EXPECT_TRUE(bytes.has_value());
    return json::parse(std::string(bytes->begin(), bytes->end()));
  }

  // Failed runs are retried while the failure is recoverable (pre-existing
  // scheduler behaviour, see test_p3_runner_context.cpp), so loop-closure
  // evidence may accumulate across attempts; every instance must still agree.
  std::vector<json> LoopClosurePayloads() {
    const auto rows =
        engine_->project().db().FindArtifactsByType("loop_closure");
    EXPECT_GE(rows.size(), 1u);
    std::vector<json> out;
    for (const auto& row : rows) {
      const auto bytes =
          engine_->project().artifacts().Get(row.content_hash);
      if (!bytes.has_value()) {
        ADD_FAILURE();
        continue;
      }
      out.push_back(json::parse(std::string(bytes->begin(), bytes->end())));
    }
    return out;
  }

  json OptimizationResultPayload() {
    const auto rows = engine_->project().db().FindArtifactsByType(
        "optimization_result");
    EXPECT_EQ(rows.size(), 1u);
    const auto bytes =
        engine_->project().artifacts().Get(rows.front().content_hash);
    EXPECT_TRUE(bytes.has_value());
    return json::parse(std::string(bytes->begin(), bytes->end()));
  }

  const json* FindImage(const json& doc, const std::string& frame_id) {
    for (const auto& img : doc["images"]) {
      if (img["frame_id"].get<std::string>() == frame_id) return &img;
    }
    return nullptr;
  }

  const json* FindOptNode(const json& payload, const std::string& frame_id) {
    for (const auto& n : payload["nodes"]) {
      if (n["frame_id"].get<std::string>() == frame_id) return &n;
    }
    return nullptr;
  }
};

// A1 — the applied revision carries the corrected poses per frame: the
// drifted target image matches the optimized node values (FrameID join),
// under a new identity, as "succeeded".
TEST_F(ApplyOptimizedTrajectoryTest, A1_AppliedPosesCorrectPerFrame) {
  std::string traj;
  ASSERT_EQ(RunDrifted("step7_a1", &traj), "succeeded");

  json applied;
  ASSERT_TRUE(FindApplied(&applied));
  EXPECT_EQ(applied["status"].get<std::string>(), "succeeded");
  const json opt = OptimizationResultPayload();
  ASSERT_EQ(opt["nodes"].size(), 2u);
  ASSERT_EQ(applied["images"].size(), 2u);

  // Per-frame values equal the optimized nodes (1e-12: same transform path).
  for (const auto& frame : {src_frame_, tgt_frame_}) {
    const json* img = FindImage(applied, frame);
    const json* node = FindOptNode(opt, frame);
    ASSERT_TRUE(img != nullptr);
    ASSERT_TRUE(node != nullptr);
    for (int i = 0; i < 3; ++i)
      EXPECT_NEAR(img->at("pose")["translation_xyz"][i].get<double>(),
                  node->at("position_xyz")[i].get<double>(), 1e-12);
    for (int i = 0; i < 4; ++i)
      EXPECT_NEAR(img->at("pose")["rotation_xyzw"][i].get<double>(),
                  node->at("rotation_xyzw")[i].get<double>(), 1e-12);
  }

  // And the drift is really gone from the reconstruction: the target image
  // sits at the TRUE canonical pose c_t (the measured baseline), not at the
  // drifted odometry pose.
  const Eigen::Vector3d expect = TrueTargetTranslation();
  const json* tgt_img = FindImage(applied, tgt_frame_);
  ASSERT_TRUE(tgt_img != nullptr);
  const Eigen::Vector3d p_tgt(
      tgt_img->at("pose")["translation_xyz"][0].get<double>(),
      tgt_img->at("pose")["translation_xyz"][1].get<double>(),
      tgt_img->at("pose")["translation_xyz"][2].get<double>());
  EXPECT_LT((p_tgt - expect).norm(), 1e-3);
  EXPECT_GT((DriftedTargetTranslation() - expect).norm(), 0.1);

  // New identity, not the committed one.
  EXPECT_FALSE(applied["reconstruction_id"].get<std::string>().empty());
  const json source_doc = json::parse(recon_doc_);
  EXPECT_NE(applied["reconstruction_id"].get<std::string>(),
            source_doc["reconstruction_id"].get<std::string>());
}

// A2 — the source is never mutated: the committed row is byte-identical to
// the input CAS bytes; the applied revision preserves points, cameras,
// image order/count/detected flags verbatim (no retriangulation here).
TEST_F(ApplyOptimizedTrajectoryTest, A2_SourceImmutable) {
  std::string traj;
  ASSERT_EQ(RunDrifted("step7_a2", &traj), "succeeded");

  const json source_doc = json::parse(recon_doc_);
  const auto scene =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  const auto rows = engine_->project().db().FindReconstructionsByScene(
      scene->scene_id);
  const json* committed = nullptr;
  for (const auto& row : rows) {
    if (row.reconstruction_id ==
        ParseUuid(
            source_doc["reconstruction_id"].get<std::string>())) {
      EXPECT_EQ(row.document_json, recon_doc_);  // byte-identical commit
      committed = &source_doc;
      break;
    }
  }
  ASSERT_TRUE(committed != nullptr);

  json applied;
  ASSERT_TRUE(FindApplied(&applied));
  EXPECT_EQ(applied["points3D"], source_doc["points3D"]);
  EXPECT_EQ(applied["cameras"], source_doc["cameras"]);
  ASSERT_EQ(applied["images"].size(), source_doc["images"].size());
  for (std::size_t i = 0; i < applied["images"].size(); ++i) {
    EXPECT_EQ(applied["images"][i]["frame_id"].get<std::string>(),
              source_doc["images"][i]["frame_id"].get<std::string>());
    EXPECT_EQ(applied["images"][i]["name"].get<std::string>(),
              source_doc["images"][i]["name"].get<std::string>());
    EXPECT_EQ(applied["images"][i]["detected"].get<bool>(),
              source_doc["images"][i]["detected"].get<bool>());
    EXPECT_EQ(applied["images"][i]["camera_id"].get<int>(),
              source_doc["images"][i]["camera_id"].get<int>());
  }
}

// A3 — provenance lineage: spatial_optimizer backend, both evidence hashes,
// result_id link, evidence-chained timestamp.
TEST_F(ApplyOptimizedTrajectoryTest, A3_ProvenanceLineage) {
  std::string traj;
  ASSERT_EQ(RunDrifted("step7_a3", &traj), "succeeded");

  json applied;
  ASSERT_TRUE(FindApplied(&applied));
  const auto& prov = applied["provenance"];
  EXPECT_EQ(prov["backend"]["name"].get<std::string>(), "spatial_optimizer");
  const json opt = OptimizationResultPayload();
  EXPECT_NE(prov["backend_specific_json"].get<std::string>().find(
                opt["result_id"].get<std::string>()),
            std::string::npos);
  std::vector<std::string> hashes;
  for (const auto& h : prov["input_artifact_hashes"])
    hashes.push_back(h.get<std::string>());
  EXPECT_NE(std::find(hashes.begin(), hashes.end(), recon_hash_),
            hashes.end());
  const auto opt_rows =
      engine_->project().db().FindArtifactsByType("optimization_result");
  ASSERT_EQ(opt_rows.size(), 1u);
  EXPECT_NE(std::find(hashes.begin(), hashes.end(),
                      opt_rows.front().content_hash),
            hashes.end());
  EXPECT_EQ(applied["created_at_ns"].get<std::int64_t>(),
            opt["created_at_ns"].get<std::int64_t>());
}

// A4 — loop-closure / graph / optimization evidence is untouched by apply.
TEST_F(ApplyOptimizedTrajectoryTest, A4_EvidenceUntouched) {
  std::string traj;
  ASSERT_EQ(RunDrifted("step7_a4", &traj), "succeeded");

  const json lc = LoopClosurePayload();
  EXPECT_TRUE(lc["closures"][0]["has_relative_pose"].get<bool>());
  const auto graphs =
      engine_->project().db().FindPoseGraphsByTrajectory(ParseUuid(traj));
  ASSERT_EQ(graphs.size(), 1u);
  const json graph_doc = json::parse(graphs[0].document_json);
  for (int i = 0; i < 3; ++i)
    EXPECT_DOUBLE_EQ(
        graph_doc["edges"][0]["relative_position_xyz"][i].get<double>(),
        lc["closures"][0]["relative_position_xyz"][i].get<double>());
  const auto opt_rows =
      engine_->project().db().FindOptimizationResultsByTrajectory(
          ParseUuid(traj));
  ASSERT_EQ(opt_rows.size(), 1u);
  EXPECT_EQ(opt_rows[0].status, "converged");
}

// A5 — frame mismatch (trajectory vs reconstruction frames differ, no
// alignment): fail closed, no applied revision; evidence persists.
TEST_F(ApplyOptimizedTrajectoryTest, A5_FrameMismatchFailsClosed) {
  std::string traj;
  ASSERT_EQ(RunDrifted("step7_a5", &traj, "reconstruction_0"), "failed");

  EXPECT_FALSE(FindApplied(nullptr));
  // Evidence persists (commit + closure + graph + converged optimization);
  // only the apply refused.
  const auto scene =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  const auto recons = engine_->project().db().FindReconstructionsByScene(
      scene->scene_id);
  ASSERT_EQ(recons.size(), 1u);
  EXPECT_EQ(recons[0].status, "succeeded");
  const auto opt_rows =
      engine_->project().db().FindOptimizationResultsByTrajectory(
          ParseUuid(traj));
  ASSERT_EQ(opt_rows.size(), 1u);
  EXPECT_EQ(opt_rows[0].status, "converged");
}

// A6 — dangling optimized node (frame absent from the reconstruction):
// fail closed, no applied revision.
TEST_F(ApplyOptimizedTrajectoryTest, A6_DanglingNodeFailsClosed) {
  DanglingNodeOptimizer dangling;
  std::string traj;
  ASSERT_EQ(RunDrifted("step7_a6", &traj, "trajectory_0", &dangling),
            "failed");

  EXPECT_FALSE(FindApplied(nullptr));
  const auto opt_rows =
      engine_->project().db().FindOptimizationResultsByTrajectory(
          ParseUuid(traj));
  ASSERT_EQ(opt_rows.size(), 1u);  // optimization persisted; apply refused
}

// A7 — empty optimized node set: fail closed, no applied revision and no
// optimization result either (runner pre-check, seam backstop untouched).
TEST_F(ApplyOptimizedTrajectoryTest, A7_EmptyNodesFailClosed) {
  EmptyNodesOptimizer empty;
  std::string traj;
  ASSERT_EQ(RunDrifted("step7_a7", &traj, "trajectory_0", &empty), "failed");

  EXPECT_FALSE(FindApplied(nullptr));
  EXPECT_TRUE(engine_->project()
                  .db()
                  .FindOptimizationResultsByTrajectory(ParseUuid(traj))
                  .empty());
}

// A8 — no optimization at all (plain run, no LC keys, no optimizer seam):
// the run succeeds with exactly the commit + BA revisions (no applied
// revision). The fundamental+LC no-graph skip is covered by O7 (P3 target).
TEST_F(ApplyOptimizedTrajectoryTest, A8_NoOptimizationNoApply) {
  Reconstruction bare;
  bare.reconstruction_id = FormatUuid(GenerateUuid());
  bare.scene_id = "22222222-2222-4222-8222-222222222222";
  bare.session_ids = {"33333333-3333-4333-8333-333333333333"};
  bare.coordinate_frame = "trajectory_0";
  bare.status = "succeeded";
  bare.created_at_ns = 100;
  bare.provenance.backend.name = "colmap";
  bare.provenance.backend.version = "v2";
  bare.provenance.configuration_hash =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  bare.cameras.push_back(PinholeCamera(1));
  ReconImage img;
  img.image_id = 1;
  img.camera_id = 1;
  img.frame_id = FormatUuid(GenerateUuid());
  img.name = "frame_0.jpg";
  img.pose.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  img.pose.translation_xyz = {1.0, 2.0, 3.0};
  img.detected = true;
  bare.images.push_back(img);
  const std::string bare_doc = ReconstructionToJson(bare);
  const std::vector<std::uint8_t> bare_bytes(bare_doc.begin(),
                                             bare_doc.end());
  ArtifactManifest bare_manifest;
  bare_manifest.artifact_uuid = GenerateUuid();
  bare_manifest.type = "reconstruction";
  bare_manifest.schema_version = 2;
  bare_manifest.producer = {"spatial-platform", "0.1.0", "test"};
  bare_manifest.creation_timestamp = Iso8601UtcNow();
  bare_manifest.file_size = static_cast<std::int64_t>(bare_bytes.size());
  const std::string bare_hash =
      project_->artifacts().Put(bare_bytes, bare_manifest).content_hash;

  auto& db = project_->db();
  auto& store = project_->artifacts();
  SparseCorrectionSeams seams;
  seams.ba_optimizer = &stub_;
  seams.matcher = &matcher_;
  seams.verifier = &fundamental_;
  seams.trajectory_optimizer = nullptr;
  engine_ = std::make_unique<Engine>(
      std::move(*project_),
      MakeSparseCorrectionRunner(db, store, seams, {}),
      SparseCorrectionProfile());
  project_.reset();
  RegisterProductionPipelines(engine_->registry());
  json cfg = {{"project_id", FormatUuid(project_id_)},
              {"scene_name", "step7_a8"},
              {"random_seed", "pinned-step6"}};
  const auto manifest = engine_->RunPipeline(kSparseCorrectionPipelineId,
                                             {bare_hash}, cfg.dump());
  ASSERT_EQ(manifest.status, "succeeded");
  EXPECT_FALSE(FindApplied(nullptr));
  const auto scene =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  // Exactly commit + BA v4: nothing applied in between.
  EXPECT_EQ(engine_->project()
                .db()
                .FindReconstructionsByScene(scene->scene_id)
                .size(),
            2u);
}

// ===========================================================================
// P3.1 Step 8 — Retriangulation (applied revision -> v3 geometry recompute).
// ===========================================================================
//
// The committed reconstruction carries DRIFTED camera poses AND 3D points
// triangulated under those drifted poses (CommittedPointsUnderDrift, wrong by
// design); the pixels are exact (rendered from the true poses). Step 7 applies
// the corrected poses; Step 8 recomputes the 3D points from the corrected
// poses + the same pixels, so the v3 geometry must land back ON the rendered
// ground truth X while the committed geometry stays far away.
class RetriangulationTest : public ApplyOptimizedTrajectoryTest {
 protected:
  bool FindRetriangulated(json* doc_out) {
    const auto scene =
        engine_->project().db().FindSceneByProject(project_id_);
    if (!scene.has_value()) return false;
    const auto rows = engine_->project().db().FindReconstructionsByScene(
        scene->scene_id);
    for (const auto& row : rows) {
      const json doc = json::parse(row.document_json);
      if (doc["provenance"]["backend"]["name"].get<std::string>() ==
          "spatial_retriangulator") {
        if (doc_out != nullptr) *doc_out = doc;
        return true;
      }
    }
    return false;
  }

  // The Step 9 v4 row: the revision carrying the injected ReconstructionOptimizer
  // backend (StubOptimizer in the RT* tests, GtsamBundleAdjustmentOptimizer in
  // the BT* tests). Returns false when no BA revision exists.
  bool FindBundleAdjusted(json* doc_out) {
    const auto scene =
        engine_->project().db().FindSceneByProject(project_id_);
    if (!scene.has_value()) return false;
    const auto rows = engine_->project().db().FindReconstructionsByScene(
        scene->scene_id);
    for (const auto& row : rows) {
      const json doc = json::parse(row.document_json);
      const std::string backend =
          doc["provenance"]["backend"]["name"].get<std::string>();
      if (backend == "stub_optimizer" ||
          backend == "spatial_gtsam_bundle_adjuster") {
        if (doc_out != nullptr) *doc_out = doc;
        return true;
      }
    }
    return false;
  }

  // Runs the full Step 2-8 chain over `committed.points` (fixed scene) and
  // stores the mean 3D distance to ground truth for the committed points and
  // for the retriangulated v3 points, in that order.
  void MeasureGeometryErrors(const RenderedScene& scene,
                             const CommittedGeometry& committed) {
    std::string traj;
    ASSERT_EQ(RunDrifted("step8_rt", &traj, "trajectory_0", nullptr,
                         committed.points, &scene),
              "succeeded");
    json v3;
    ASSERT_TRUE(FindRetriangulated(&v3));
    ASSERT_EQ(v3["points3D"].size(), committed.points.size());
    committed_err_ = 0.0;
    v3_err_ = 0.0;
    for (std::size_t i = 0; i < committed.points.size(); ++i) {
      const Eigen::Vector3d truth = committed.truth[i];
      const Eigen::Vector3d committed_pos(committed.points[i].xyz[0],
                                          committed.points[i].xyz[1],
                                          committed.points[i].xyz[2]);
      const json& p = v3["points3D"][i];
      const Eigen::Vector3d v3_pos(p["xyz"][0].get<double>(),
                                   p["xyz"][1].get<double>(),
                                   p["xyz"][2].get<double>());
      committed_err_ += (committed_pos - truth).norm();
      v3_err_ += (v3_pos - truth).norm();
    }
    committed_err_ /= static_cast<double>(committed.points.size());
    v3_err_ /= static_cast<double>(committed.points.size());
  }

  double committed_err_ = 0.0;
  double v3_err_ = 0.0;
};

// RT1 — REAL geometry correction: the committed points (triangulated under the
// drifted poses) are objectively wrong versus the rendered ground truth, and
// the retriangulated v3 points are back ON the truth. All numbers are computed
// from the pinned fixture, never hardcoded.
TEST_F(RetriangulationTest, RT1_CorrectedGeometryEmerged) {
  const RenderedScene scene = RenderScene(36, 7u);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  ASSERT_GT(committed.points.size(), 10u);  // fixture sanity: DLT under drift

  MeasureGeometryErrors(scene, committed);
  EXPECT_GT(committed_err_, 0.05);   // there WAS error to remove
  EXPECT_LT(v3_err_, 0.02);          // corrected geometry: within 2 cm of truth
  EXPECT_LT(v3_err_, committed_err_ * 0.2);  // strictly better, not just noise

  // Lineage classification: every moved point is REPLACED with a fresh id
  // (geometry moved >> the 1 cm stability threshold), nothing preserved.
  json v3;
  std::string traj;
  // (re-derive the v3 doc for the classification asserts; run already done)
  ASSERT_TRUE(FindRetriangulated(&v3));
  const json counts =
      json::parse(v3["provenance"]["backend_specific_json"].get<std::string>());
  EXPECT_EQ(counts["preserved"].get<std::int64_t>(), 0);
  EXPECT_EQ(counts["replaced"].get<std::int64_t>(),
            static_cast<std::int64_t>(committed.points.size()));
  EXPECT_EQ(counts["new"].get<std::int64_t>(), 0);
  EXPECT_EQ(counts["rejected"].get<std::int64_t>(), 0);
}

// RT2 — supersede + revision rows + clock (Step 9 chain): Step 8 supersedes the
// APPLIED revision (its direct source) in one transaction; Step 9 chain-compacts
// the committed revision (its root role is fulfilled once the corrected chain
// materialized v3), then BA supersedes its own source (v3) by P14. Row set:
// committed(superseded) + applied(superseded) + v3(superseded) + v4(succeeded)
// — exactly ONE succeeded revision; timestamps strictly increasing with the
// documented +100 clock extension on the frozen origins.
TEST_F(RetriangulationTest, RT2_SupersedeClockAndRows) {
  const RenderedScene scene = RenderScene(24, 11u);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  std::string traj;
  ASSERT_EQ(RunDrifted("step8_rt2", &traj, "trajectory_0", nullptr,
                       committed.points, &scene),
            "succeeded");

  json applied;
  json v3;
  json v4;
  ASSERT_TRUE(FindApplied(&applied));
  ASSERT_TRUE(FindRetriangulated(&v3));
  ASSERT_TRUE(FindBundleAdjusted(&v4));
  const json committed_doc = json::parse(recon_doc_);

  const auto scene_row =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene_row.has_value());
  const auto rows = engine_->project().db().FindReconstructionsByScene(
      scene_row->scene_id);
  ASSERT_EQ(rows.size(), 4u);

  std::map<std::string, std::string> status_by_id;
  for (const auto& row : rows)
    status_by_id[FormatUuid(row.reconstruction_id)] = row.status;
  EXPECT_EQ(status_by_id[committed_doc["reconstruction_id"].get<std::string>()],
            "superseded");  // Step 9 chain compaction (chain root)
  EXPECT_EQ(status_by_id[applied["reconstruction_id"].get<std::string>()],
            "superseded");  // Step 8 supersedes its direct source
  EXPECT_EQ(status_by_id[v3["reconstruction_id"].get<std::string>()],
            "superseded");  // BA P14 supersedes its own source (v3)
  EXPECT_EQ(status_by_id[v4["reconstruction_id"].get<std::string>()],
            "succeeded");   // the sole succeeded revision of the scene
  {
    int succeeded = 0;
    for (const auto& entry : status_by_id)
      if (entry.second == "succeeded") ++succeeded;
    EXPECT_EQ(succeeded, 1);
  }

  // Strictly increasing revision clock, with the frozen function's 0 origin
  // stamped by the runner as applied + 100 (Step 8) and v3 + 100 (Step 9).
  EXPECT_LT(committed_doc["created_at_ns"].get<std::int64_t>(),
            applied["created_at_ns"].get<std::int64_t>());
  EXPECT_EQ(v3["created_at_ns"].get<std::int64_t>(),
            applied["created_at_ns"].get<std::int64_t>() + 100);
  EXPECT_EQ(v4["created_at_ns"].get<std::int64_t>(),
            v3["created_at_ns"].get<std::int64_t>() + 100);
  EXPECT_NE(v3["reconstruction_id"].get<std::string>(),
            applied["reconstruction_id"].get<std::string>());
  EXPECT_NE(v4["reconstruction_id"].get<std::string>(),
            v3["reconstruction_id"].get<std::string>());
}

// RT3 — verbatim pass-through + lineage: retriangulation recomputes ONLY the
// 3D points. Images (poses included), cameras, and point tracks must be
// byte-identical to the applied revision; the backend is the frozen
// spatial_retriangulator with the observation-chain lineage in the document.
TEST_F(RetriangulationTest, RT3_VerbatimAndLineage) {
  const RenderedScene scene = RenderScene(24, 13u);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  std::string traj;
  ASSERT_EQ(RunDrifted("step8_rt3", &traj, "trajectory_0", nullptr,
                       committed.points, &scene),
            "succeeded");
  json applied;
  json v3;
  ASSERT_TRUE(FindApplied(&applied));
  ASSERT_TRUE(FindRetriangulated(&v3));

  EXPECT_EQ(v3["provenance"]["backend"]["name"].get<std::string>(),
            "spatial_retriangulator");
  // Poses/cameras are geometry INPUTS on this boundary, never outputs.
  EXPECT_EQ(v3["images"], applied["images"]);
  EXPECT_EQ(v3["cameras"], applied["cameras"]);
  // Points are the ONLY recomputed geometry; tracks pass through verbatim.
  ASSERT_EQ(v3["points3D"].size(), applied["points3D"].size());
  for (std::size_t i = 0; i < applied["points3D"].size(); ++i)
    EXPECT_EQ(v3["points3D"][i]["track"], applied["points3D"][i]["track"]);

  // Document lineage: the frozen Retriangulate chains onto the applied
  // revision (its revision id + the applied revision's own input hashes).
  std::vector<std::string> v3_hashes;
  for (const auto& h : v3["provenance"]["input_artifact_hashes"])
    v3_hashes.push_back(h.get<std::string>());
  EXPECT_NE(std::find(v3_hashes.begin(), v3_hashes.end(),
                      applied["reconstruction_id"].get<std::string>()),
            v3_hashes.end());
  for (const auto& h : applied["provenance"]["input_artifact_hashes"])
    EXPECT_NE(std::find(v3_hashes.begin(), v3_hashes.end(),
                        h.get<std::string>()),
              v3_hashes.end());
}

// RT4 — observation resolution reads the canonical CAS feature artifacts (the
// same source the loop-closure stage used, per frame_id -> feature_set chain),
// and the Step-8 evidence does not mutate the Step 2-7 evidence. The resolved
// observation artifacts' manifests must exist in the store with the hashes the
// fixture recorded when writing them.
TEST_F(RetriangulationTest, RT4_ObservationsFromCanonicalArtifacts) {
  const RenderedScene scene = RenderScene(24, 17u);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  std::string traj;
  ASSERT_EQ(RunDrifted("step8_rt4", &traj, "trajectory_0", nullptr,
                       committed.points, &scene),
            "succeeded");
  json v3;
  ASSERT_TRUE(FindRetriangulated(&v3));

  auto& store = engine_->project().artifacts();
  const auto src_manifest = store.ReadManifest(ParseUuid(src_art_));
  const auto tgt_manifest = store.ReadManifest(ParseUuid(tgt_art_));
  ASSERT_TRUE(src_manifest.has_value());
  ASSERT_TRUE(tgt_manifest.has_value());
  EXPECT_EQ(src_manifest->content_hash, src_art_hash_);
  EXPECT_EQ(tgt_manifest->content_hash, tgt_art_hash_);

  // The whole measurement->apply evidence chain stays untouched by Step 8.
  const json lc = LoopClosurePayload();
  EXPECT_TRUE(lc["closures"][0]["has_relative_pose"].get<bool>());
  const auto graphs =
      engine_->project().db().FindPoseGraphsByTrajectory(ParseUuid(traj));
  ASSERT_EQ(graphs.size(), 1u);
  const auto opt_rows =
      engine_->project().db().FindOptimizationResultsByTrajectory(
          ParseUuid(traj));
  ASSERT_EQ(opt_rows.size(), 1u);
  EXPECT_EQ(opt_rows[0].status, "converged");
}

// RT5 — missing feature set fails closed: a tracked image whose frame has no
// feature_sets row (and no feature_artifacts pin) cannot resolve observations.
// The runner throws, no v3 is persisted, and the applied revision is left
// succeeded (nothing half-persisted — P12).
TEST_F(RetriangulationTest, RT5_MissingFeatureSetFailsClosed) {
  const RenderedScene scene = RenderScene(24, 19u);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  std::string traj;
  ASSERT_EQ(RunDrifted("step8_rt5", &traj, "trajectory_0", nullptr,
                       committed.points, &scene,
                       /*register_feature_sets=*/false),
            "failed");

  EXPECT_FALSE(FindRetriangulated(nullptr));
  json applied;
  ASSERT_TRUE(FindApplied(&applied));
  EXPECT_EQ(applied["status"].get<std::string>(), "succeeded");
}

// RT6 — untriangulable tracks fail closed: every committed point carries a
// single-element track, the frozen predicate rejects all of them, and the
// runner refuses to persist an empty-geometry revision.
TEST_F(RetriangulationTest, RT6_SingleElementTracksFailClosed) {
  const RenderedScene scene = RenderScene(24, 23u);
  std::vector<ReconPoint3D> single_track;
  for (std::size_t i = 0; i < scene.X.size(); ++i) {
    ReconPoint3D pt;
    pt.point3d_id = static_cast<std::uint64_t>(i + 1);
    pt.xyz = {0.5, 0.0, 4.0};
    pt.track = {{1u, static_cast<std::int32_t>(i)}};
    single_track.push_back(std::move(pt));
  }
  std::string traj;
  ASSERT_EQ(RunDrifted("step8_rt6", &traj, "trajectory_0", nullptr,
                       single_track, &scene),
            "failed");
  EXPECT_FALSE(FindRetriangulated(nullptr));
  json applied;
  ASSERT_TRUE(FindApplied(&applied));
  EXPECT_EQ(applied["status"].get<std::string>(), "succeeded");
}

// RT7 — out-of-range point2d_idx fails closed: the track index is checked
// against the resolved feature artifact bounds (fail-closed, no fallback).
TEST_F(RetriangulationTest, RT7_OutOfRangeIndexFailsClosed) {
  const RenderedScene scene = RenderScene(24, 29u);
  ReconPoint3D bad;
  bad.point3d_id = 99;
  bad.xyz = {0.5, 0.0, 4.0};
  bad.track = {{1u, 0}, {2u, 100000}};  // second index outside the artifact
  std::string traj;
  ASSERT_EQ(RunDrifted("step8_rt7", &traj, "trajectory_0", nullptr,
                       {bad}, &scene),
            "failed");
  EXPECT_FALSE(FindRetriangulated(nullptr));
  json applied;
  ASSERT_TRUE(FindApplied(&applied));
  EXPECT_EQ(applied["status"].get<std::string>(), "succeeded");
}

// RT8 — no committed points is vacuous: Step 8 skips (no fabrication of a v3),
// the applied revision stays the only spatial_optimizer row and remains
// succeeded; the run itself succeeds.
TEST_F(RetriangulationTest, RT8_NoPointsSkipsRetriangulation) {
  std::string traj;
  ASSERT_EQ(RunDrifted("step8_rt8", &traj), "succeeded");
  EXPECT_FALSE(FindRetriangulated(nullptr));
  json applied;
  ASSERT_TRUE(FindApplied(&applied));
  EXPECT_EQ(applied["status"].get<std::string>(), "succeeded");
  // Only committed + applied + BA v4: no v3 anywhere.
  const auto scene =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  EXPECT_EQ(engine_->project()
                .db()
                .FindReconstructionsByScene(scene->scene_id)
                .size(),
            3u);
}

// ===========================================================================
// P3.1 Step 9 — Bundle Adjustment (v3 -> v4) through the REAL GTSAM seam.
// ===========================================================================
//
// Same full corrected chain as the RT* family, but the ReconstructionOptimizer
// seam is the REAL GtsamBundleAdjustmentOptimizer (batch LM, fixed intrinsics,
// Huber loss) instead of the StubOptimizer. The pixels carry a small noise σ so
// the retriangulated v3 keeps finite residuals and the frozen D5 gate has a
// measurable before->after improvement to accept (an exact-pixels + exact-poses
// v3 is already residual-zero — nothing for LM to refine). The run "succeeded"
// manifest status IS the D5 gate verdict (the runner throws ValidationError on
// gate failure), so BT1's success assertion proves the gate accepted the v4;
// the optimizer telemetry is asserted to report the same rms_after < rms_before
// signal. 3D proximity to ground truth is sanity-bounded only: the LM contract
// minimizes 2D reprojection error, which can trade a few millimetres of world
// accuracy against the sensor noise.
class RealGtsamBundleAdjustmentTest : public RetriangulationTest {
 protected:
  // Mean 3D distance (m) of a persisted revision's points to the rendered
  // ground truth, in reconstruction-frame coordinates (source camera at the
  // origin). The i-th row of the revision corresponds to the i-th row of the
  // committed geometry: frozen 8b emits its ACCEPTED source points in source
  // order but renumbers the replaced ones with fresh ids, so id-keyed lookup
  // is not stable; index alignment is (RT1 relies on the same premise).
  double MeanDistanceToTruth(const CommittedGeometry& committed,
                             const json& revision) {
    EXPECT_EQ(revision["points3D"].size(), committed.points.size());
    const std::size_t n =
        std::min(revision["points3D"].size(), committed.points.size());
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const Eigen::Vector3d xyz(revision["points3D"][i]["xyz"][0].get<double>(),
                                revision["points3D"][i]["xyz"][1].get<double>(),
                                revision["points3D"][i]["xyz"][2].get<double>());
      total += (xyz - committed.truth[i]).norm();
    }
    return n > 0 ? total / static_cast<double>(n) : 0.0;
  }

  GtsamBundleAdjustmentOptimizer real_ba_;
};

// BT1 — the REAL backend refines the v3 geometry (not an idempotent passthrough)
// and every chain invariant holds under that refinement: exactly one succeeded
// revision (v4), all three predecessors superseded, the +100 clock extension,
// and byte-identical intrinsics (D3). Geometry quality versus the rendered
// ground truth is preserved or improved by the refinement.
TEST_F(RealGtsamBundleAdjustmentTest, BT1_RealBundleAdjustmentRefinesV3) {
  const RenderedScene scene = RenderScene(36, 7u, /*noise_px=*/0.4);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  ASSERT_GT(committed.points.size(), 10u);  // fixture sanity

  std::string traj;
  ASSERT_EQ(RunDrifted("step9_bt1", &traj, "trajectory_0", nullptr,
                       committed.points, &scene, /*register_feature_sets=*/true,
                       &real_ba_),
            "succeeded");

  json applied;
  json v3;
  json v4;
  ASSERT_TRUE(FindApplied(&applied));
  ASSERT_TRUE(FindRetriangulated(&v3));
  ASSERT_TRUE(FindBundleAdjusted(&v4));
  const json committed_doc = json::parse(recon_doc_);

  EXPECT_EQ(v4["status"].get<std::string>(), "succeeded");
  EXPECT_EQ(v4["created_at_ns"].get<std::int64_t>(),
            v3["created_at_ns"].get<std::int64_t>() + 100);
  EXPECT_NE(v4["reconstruction_id"].get<std::string>(),
            v3["reconstruction_id"].get<std::string>());
  EXPECT_EQ(v4["scene_id"].get<std::string>(),
            v3["scene_id"].get<std::string>());
  EXPECT_NE(v4["provenance"]["backend"]["name"].get<std::string>(),
            "spatial_retriangulator");
  EXPECT_EQ(v4["provenance"]["backend"]["name"].get<std::string>(),
            "spatial_gtsam_bundle_adjuster");
  EXPECT_FALSE(v4["provenance"]["configuration_hash"].get<std::string>()
                   .empty());
  const json ba_telemetry = json::parse(
      v4["provenance"]["backend_specific_json"].get<std::string>());
  EXPECT_EQ(ba_telemetry["optimizer"].get<std::string>(),
            "LevenbergMarquardt");
  EXPECT_TRUE(ba_telemetry["converged"].get<bool>());
  EXPECT_GT(ba_telemetry["iterations"].get<std::int64_t>(), 0);
  // The D5 gate (which the "succeeded" run status above proves) accepted the
  // v4 because the frozen evaluator measured a real before->after reprojection
  // improvement; the telemetry must reflect the same signal: finite residuals
  // and rms_after strictly below rms_before.
  EXPECT_GT(ba_telemetry["rms_before"].get<double>(), 0.0);
  EXPECT_GT(ba_telemetry["rms_after"].get<double>(), 0.0);
  EXPECT_LT(ba_telemetry["rms_after"].get<double>(),
            ba_telemetry["rms_before"].get<double>());

  // D3: fixed intrinsics are reproduced byte-identically in the v4 document.
  EXPECT_EQ(v4["cameras"], v3["cameras"]);

  // The refinement is REAL: the v4 geometry is not a bitwise copy of the v3
  // payload (the LM terms perturb the retriangulated points). Proximity to
  // ground truth is sanity-bounded only: the frozen contract minimizes 2D
  // reprojection error (above), which may trade a few millimetres of world
  // accuracy against the sensor noise — never the absence of a firing LM.
  bool differs = false;
  ASSERT_EQ(v4["points3D"].size(), v3["points3D"].size());
  for (std::size_t i = 0; i < v3["points3D"].size(); ++i) {
    if (v4["points3D"][i]["xyz"] != v3["points3D"][i]["xyz"]) differs = true;
  }
  EXPECT_TRUE(differs);
  const double v3_err = MeanDistanceToTruth(committed, v3);
  const double v4_err = MeanDistanceToTruth(committed, v4);
  EXPECT_GT(v3_err, 1e-6);        // noisy keypoints: v3 is NOT bit-on the truth
  EXPECT_LT(v4_err, 10.0 * v3_err);  // sanity: no catastrophic divergence

  // Chain invariants under the real refinement (mirror of RT2's status layout):
  // committed (root, compacted) + applied (superseded by v3) + v3 (superseded
  // by v4) superseded; exactly v4 succeeded.
  const auto scene_row =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene_row.has_value());
  const auto rows = engine_->project().db().FindReconstructionsByScene(
      scene_row->scene_id);
  ASSERT_EQ(rows.size(), 4u);
  {
    std::map<std::string, std::string> status_by_id;
    for (const auto& row : rows)
      status_by_id[FormatUuid(row.reconstruction_id)] = row.status;
    EXPECT_EQ(status_by_id[committed_doc["reconstruction_id"].get<std::string>()],
              "superseded");
    EXPECT_EQ(status_by_id[applied["reconstruction_id"].get<std::string>()],
              "superseded");
    EXPECT_EQ(status_by_id[v3["reconstruction_id"].get<std::string>()],
              "superseded");
    EXPECT_EQ(status_by_id[v4["reconstruction_id"].get<std::string>()],
              "succeeded");
    int succeeded = 0;
    for (const auto& entry : status_by_id)
      if (entry.second == "succeeded") ++succeeded;
    EXPECT_EQ(succeeded, 1);
  }
}

// ===========================================================================
// FINAL PRODUCTION E2E — the ENTIRE correction chain driven by ONE
// Engine::RunPipeline("p3_sparse_correction", ...) invocation with ALL seams
// real (L2NearestMatcher + EssentialGeometricVerifier +
// GtsamTrajectoryOptimizer + GtsamBundleAdjustmentOptimizer). No host-stage
// function is called directly anywhere below — the engine surface is the only
// entry point. Stage 1 runs in reconstruction mode (CAS passthrough of the
// committed sparse model): the images-mode worker subprocess cannot feed the
// corrected chain because its output carries no frame->feature linkage (the
// known gap the golden suite covers separately with the stub BA seam).
// ===========================================================================

// E2E1 — POSITIVE metric-basis path in one invocation. Real visual loop
// closure -> metric edge -> PoseGraph -> real GTSAM trajectory optimization ->
// apply -> retriangulated v3 -> real GTSAM BA -> v4. Every link's evidence is
// walked and asserted (measurement -> graph -> optimization -> applied -> v3 ->
// v4), the geometry really changes (trajectory drift removed, points
// re-computed onto the rendered truth, BA refinement with rms_after<rms_before
// and byte-identical intrinsics), and the persistence contract holds:
// committed/applied/v3 all superseded, v4 the SOLE succeeded/latest revision,
// with the lineage chain chained revision-to-revision.
TEST_F(RealGtsamBundleAdjustmentTest, E2E1_FullProductionChainSingleInvocation) {
  const RenderedScene scene = RenderScene(36, 7u, /*noise_px=*/0.4);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  ASSERT_GT(committed.points.size(), 10u);

  std::string traj;
  spatial::engine::ExecutionManifest manifest;
  ASSERT_EQ(RunDrifted("e2e_pos", &traj, "trajectory_0", nullptr,
                       committed.points, &scene, /*register_feature_sets=*/true,
                       &real_ba_, /*metric_basis_declared=*/true, &manifest),
            "succeeded");
  ASSERT_EQ(manifest.stages.size(), 2u);
  EXPECT_EQ(manifest.stages[0].status, "succeeded");  // sparse reconstruction
  EXPECT_EQ(manifest.stages[1].status, "succeeded");  // correct: chain + BA

  // (1) LOOP CLOSURE — real matcher + essential verification + metric
  //     resolution: accepted, inliers, unit->metre relative pose on the TRUE
  //     0.65 m baseline.
  const json lc = LoopClosurePayload();
  ASSERT_EQ(lc["closures"].size(), 1u);
  const auto& closure = lc["closures"][0];
  EXPECT_EQ(closure["status"].get<std::string>(), "accepted");
  EXPECT_TRUE(closure["has_relative_pose"].get<bool>());
  EXPECT_GT(closure["inlier_count"].get<std::int64_t>(), 0);
  Eigen::Vector3d rel_pos(closure["relative_position_xyz"][0].get<double>(),
                          closure["relative_position_xyz"][1].get<double>(),
                          closure["relative_position_xyz"][2].get<double>());
  EXPECT_NEAR(rel_pos.norm(), GroundTruthTranslation().norm(), 5e-2);

  // (2) POSEGRAPH — the persisted metric edge is the verbatim measurement.
  const auto graphs =
      engine_->project().db().FindPoseGraphsByTrajectory(ParseUuid(traj));
  ASSERT_EQ(graphs.size(), 1u);
  const json graph_doc = json::parse(graphs[0].document_json);
  ASSERT_EQ(graph_doc["edges"].size(), 1u);
  for (int i = 0; i < 3; ++i)
    EXPECT_DOUBLE_EQ(
        graph_doc["edges"][0]["relative_position_xyz"][i].get<double>(),
        closure["relative_position_xyz"][i].get<double>());

  // (3) OPTIMIZATION — real GTSAM, converged, error reduced, node moved off
  //     the drift, result referencing the same graph.
  const auto opt_rows =
      engine_->project().db().FindOptimizationResultsByTrajectory(
          ParseUuid(traj));
  ASSERT_EQ(opt_rows.size(), 1u);
  EXPECT_EQ(opt_rows[0].status, "converged");
  EXPECT_GT(opt_rows[0].initial_error, 0.0);
  EXPECT_LT(opt_rows[0].final_error, opt_rows[0].initial_error);
  const json opt = OptimizationPayload();
  EXPECT_EQ(opt["graph_id"].get<std::string>(),
            graph_doc["graph_id"].get<std::string>());
  EXPECT_EQ(opt["trajectory_id"].get<std::string>(), traj);
  const Eigen::Vector3d drifted = DriftedTargetTranslation();
  {
    Eigen::Vector3d p_tgt(0, 0, 0);
    bool found = false;
    for (const auto& n : opt["nodes"]) {
      if (n["frame_id"].get<std::string>() == tgt_frame_) {
        p_tgt = Eigen::Vector3d(n["position_xyz"][0].get<double>(),
                                n["position_xyz"][1].get<double>(),
                                n["position_xyz"][2].get<double>());
        found = true;
      }
    }
    ASSERT_TRUE(found);
    EXPECT_GT((p_tgt - drifted).norm(), 0.05);  // the drift was really removed
  }

  // (4) APPLIED REVISION — spatial_optimizer backend, per-frame FrameID join
  //     with the optimized nodes, target closer to the TRUE baseline than the
  //     drift, provenance chained onto the committed revision + the result.
  json applied;
  ASSERT_TRUE(FindApplied(&applied));
  EXPECT_EQ(applied["status"].get<std::string>(), "succeeded");
  EXPECT_EQ(applied["provenance"]["backend"]["name"].get<std::string>(),
            "spatial_optimizer");
  ASSERT_EQ(opt["nodes"].size(), 2u);
  ASSERT_EQ(applied["images"].size(), 2u);
  for (const auto& frame : {src_frame_, tgt_frame_}) {
    const json* img = FindImage(applied, frame);
    const json* node = FindOptNode(opt, frame);
    ASSERT_TRUE(img != nullptr);
    ASSERT_TRUE(node != nullptr);
    for (int i = 0; i < 3; ++i)
      EXPECT_NEAR(img->at("pose")["translation_xyz"][i].get<double>(),
                  node->at("position_xyz")[i].get<double>(), 1e-12);
    for (int i = 0; i < 4; ++i)
      EXPECT_NEAR(img->at("pose")["rotation_xyzw"][i].get<double>(),
                  node->at("rotation_xyzw")[i].get<double>(), 1e-12);
  }
  const Eigen::Vector3d true_t = TrueTargetTranslation();
  const json* tgt_img = FindImage(applied, tgt_frame_);
  ASSERT_TRUE(tgt_img != nullptr);
  const Eigen::Vector3d applied_t(
      tgt_img->at("pose")["translation_xyz"][0].get<double>(),
      tgt_img->at("pose")["translation_xyz"][1].get<double>(),
      tgt_img->at("pose")["translation_xyz"][2].get<double>());
  EXPECT_LT((applied_t - true_t).norm(), (drifted - true_t).norm());
  EXPECT_LT((applied_t - true_t).norm(), 0.05);
  // The correction includes the ROTATION: the applied target orientation is
  // closer to the truth than the drifted input (real GTSAM trajectory
  // optimization), mirroring the translation claim above.
  EXPECT_LT(RotationAngle(QuatToMatrix(tgt_img->at("pose")["rotation_xyzw"]),
                          TrueTargetRotation()),
            RotationAngle(DriftedTargetRotation(), TrueTargetRotation()));
  // Lineage: applied chains onto the committed artifact + the optimization.
  std::vector<std::string> applied_hashes;
  for (const auto& h : applied["provenance"]["input_artifact_hashes"])
    applied_hashes.push_back(h.get<std::string>());
  EXPECT_NE(std::find(applied_hashes.begin(), applied_hashes.end(),
                      recon_hash_),
            applied_hashes.end());
  const auto opt_rows2 =
      engine_->project().db().FindArtifactsByType("optimization_result");
  ASSERT_EQ(opt_rows2.size(), 1u);
  EXPECT_NE(std::find(applied_hashes.begin(), applied_hashes.end(),
                      opt_rows2.front().content_hash),
            applied_hashes.end());
  EXPECT_NE(applied["provenance"]["backend_specific_json"].get<std::string>()
                .find(opt["result_id"].get<std::string>()),
            std::string::npos);

  // (5) RETRIANGULATED v3 — the SAME pixels, re-triangulated from the
  //     corrected poses: the geometry is recomputed (even point tracks
  //     preserved) and lands CLOSER to the truth than the drifted committed
  //     revision (the exact-pixel accuracy level is proven separately by RT1);
  //     images/cameras verbatim, lineage onto the applied revision.
  json v3;
  ASSERT_TRUE(FindRetriangulated(&v3));
  EXPECT_EQ(v3["provenance"]["backend"]["name"].get<std::string>(),
            "spatial_retriangulator");
  EXPECT_EQ(v3["images"], applied["images"]);
  EXPECT_EQ(v3["cameras"], applied["cameras"]);
  ASSERT_EQ(v3["points3D"].size(), applied["points3D"].size());
  for (std::size_t i = 0; i < applied["points3D"].size(); ++i)
    EXPECT_EQ(v3["points3D"][i]["track"], applied["points3D"][i]["track"]);
  double committed_err = 0.0;
  for (std::size_t i = 0; i < committed.points.size(); ++i) {
    const Eigen::Vector3d truth = committed.truth[i];
    const Eigen::Vector3d committed_pos(committed.points[i].xyz[0],
                                        committed.points[i].xyz[1],
                                        committed.points[i].xyz[2]);
    committed_err += (committed_pos - truth).norm();
  }
  committed_err /= static_cast<double>(committed.points.size());
  const double v3_err = MeanDistanceToTruth(committed, v3);
  EXPECT_GT(committed_err, 0.05);  // the committed 3D points were really wrong
  EXPECT_GT(v3_err, 1e-6);         // v3 really moved the geometry
  EXPECT_LT(v3_err, committed_err);  // ... and it moved TOWARD the truth

  // (6) REAL BA v4 — spatial_gtsam_bundle_adjuster, LM telemetry with
  //     rms_after < rms_before, D3 intrinsics byte-identical, +100 clock, and
  //     a geometry that really changed.
  json v4;
  ASSERT_TRUE(FindBundleAdjusted(&v4));
  EXPECT_EQ(v4["status"].get<std::string>(), "succeeded");
  EXPECT_EQ(v4["provenance"]["backend"]["name"].get<std::string>(),
            "spatial_gtsam_bundle_adjuster");
  EXPECT_EQ(v4["created_at_ns"].get<std::int64_t>(),
            v3["created_at_ns"].get<std::int64_t>() + 100);
  EXPECT_EQ(v4["cameras"], v3["cameras"]);
  const json ba_telemetry = json::parse(
      v4["provenance"]["backend_specific_json"].get<std::string>());
  EXPECT_EQ(ba_telemetry["optimizer"].get<std::string>(),
            "LevenbergMarquardt");
  EXPECT_TRUE(ba_telemetry["converged"].get<bool>());
  EXPECT_GT(ba_telemetry["iterations"].get<std::int64_t>(), 0);
  EXPECT_GT(ba_telemetry["rms_before"].get<double>(), 0.0);
  EXPECT_GT(ba_telemetry["rms_after"].get<double>(), 0.0);
  EXPECT_LT(ba_telemetry["rms_after"].get<double>(),
            ba_telemetry["rms_before"].get<double>());
  ASSERT_EQ(v4["points3D"].size(), v3["points3D"].size());
  {
    bool differs = false;
    for (std::size_t i = 0; i < v3["points3D"].size(); ++i)
      if (v4["points3D"][i]["xyz"] != v3["points3D"][i]["xyz"]) differs = true;
    EXPECT_TRUE(differs);
  }
  const double v4_err = MeanDistanceToTruth(committed, v4);
  EXPECT_GT(v3_err, 1e-6);
  EXPECT_LT(v4_err, 10.0 * v3_err);

  // (7) PERSISTENCE — committed / applied / v3 all superseded, v4 the SOLE
  //     succeeded (and therefore latest) revision.
  const auto scene_row =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene_row.has_value());
  const auto rows = engine_->project().db().FindReconstructionsByScene(
      scene_row->scene_id);
  ASSERT_EQ(rows.size(), 4u);
  const json committed_doc = json::parse(recon_doc_);
  std::map<std::string, std::string> status_by_id;
  for (const auto& row : rows)
    status_by_id[FormatUuid(row.reconstruction_id)] = row.status;
  EXPECT_EQ(
      status_by_id[committed_doc["reconstruction_id"].get<std::string>()],
      "superseded");
  EXPECT_EQ(status_by_id[applied["reconstruction_id"].get<std::string>()],
            "superseded");
  EXPECT_EQ(status_by_id[v3["reconstruction_id"].get<std::string>()],
            "superseded");
  EXPECT_EQ(status_by_id[v4["reconstruction_id"].get<std::string>()],
            "succeeded");
  int succeeded = 0;
  for (const auto& entry : status_by_id)
    if (entry.second == "succeeded") ++succeeded;
  EXPECT_EQ(succeeded, 1);
  const auto latest =
      engine_->project().db().QueryLatestReconstructionByScene(
          scene_row->scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id,
            ParseUuid(v4["reconstruction_id"].get<std::string>()));

  // (8) SOURCE EVIDENCE — the same registered feature artifacts the loop
  //     closure consumed fed the retriangulation observations (frame ->
  //     feature_set -> CAS), untouched.
  auto& store = engine_->project().artifacts();
  const auto src_manifest = store.ReadManifest(ParseUuid(src_art_));
  const auto tgt_manifest = store.ReadManifest(ParseUuid(tgt_art_));
  ASSERT_TRUE(src_manifest.has_value());
  ASSERT_TRUE(tgt_manifest.has_value());
  EXPECT_EQ(src_manifest->content_hash, src_art_hash_);
  EXPECT_EQ(tgt_manifest->content_hash, tgt_art_hash_);
}

// E2E2 — NEGATIVE no-metric-basis path in one invocation. The visual loop
// closure VERIFIES (real matcher + essential PASS, inliers) but the basis is
// by-fiat (undeclared), so INV-3 refuses the metric edge: NO PoseGraph, NO
// optimization (the real GTSAM optimizer is not even invoked), NO applied
// revision, NO retriangulation. With a REAL bundle-adjustment seam the plain
// commit path then has NO observations to refine and the strict D5 gate
// refuses the vacuous no-op — the pipeline FAILS cleanly (P12) and the
// committed reconstruction remains the ONLY succeeded/latest revision: nothing
// superseded, nothing corrected. The essence of INV-3 is the causal assertion,
// not the manifest status; every check below keys the absence of loop-derived
// evidence.
TEST_F(RealGtsamBundleAdjustmentTest, E2E2_NoMetricBasisYieldsNoCorrection) {
  const RenderedScene scene = RenderScene(24, 31u, /*noise_px=*/0.4);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  ASSERT_GT(committed.points.size(), 10u);

  std::string traj;
  spatial::engine::ExecutionManifest manifest;
  // declared=false -> by-fiat (ineligible) basis; all seams REAL elsewhere.
  ASSERT_EQ(RunDrifted("e2e_neg", &traj, "trajectory_0", nullptr,
                       committed.points, &scene, /*register_feature_sets=*/true,
                       &real_ba_, /*metric_basis_declared=*/false, &manifest),
            "failed");
  ASSERT_EQ(manifest.stages.size(), 2u);
  EXPECT_EQ(manifest.stages[0].status, "succeeded");  // sparse reconstruction
  EXPECT_EQ(manifest.stages[1].status, "failed");     // D5 gate refusal

  // Essential PASS but visual-only: the loop closed geometrically, the basis
  // gate refused a METRIC edge (INV-3). A failed run is retried while the
  // failure is recoverable, so the closure may persist more than once — every
  // instance must be accepted and visual-only.
  const auto lcs = LoopClosurePayloads();
  ASSERT_GE(lcs.size(), 1u);
  for (const auto& lc : lcs) {
    ASSERT_EQ(lc["closures"].size(), 1u);
    EXPECT_EQ(lc["closures"][0]["status"].get<std::string>(), "accepted");
    EXPECT_GT(lc["closures"][0]["inlier_count"].get<std::int64_t>(), 0);
    EXPECT_FALSE(lc["closures"][0]["has_relative_pose"].get<bool>());
  }

  // NO metric edge -> NO PoseGraph -> NO optimization -> NO apply -> NO v3.
  EXPECT_TRUE(
      engine_->project().db().FindPoseGraphsByTrajectory(ParseUuid(traj))
          .empty());
  EXPECT_TRUE(
      engine_->project().db().FindOptimizationResultsByTrajectory(
          ParseUuid(traj))
          .empty());
  EXPECT_FALSE(FindApplied(nullptr));
  EXPECT_FALSE(FindRetriangulated(nullptr));
  EXPECT_FALSE(FindBundleAdjusted(nullptr));

  // Persistence: the committed reconstruction is the ONLY revision — SUCCEEDED
  // and latest, byte-identical to the CAS input. Nothing corrected, nothing
  // superseded.
  const auto scene_row =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene_row.has_value());
  const auto rows = engine_->project().db().FindReconstructionsByScene(
      scene_row->scene_id);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].status, "succeeded");
  EXPECT_EQ(rows[0].document_json, recon_doc_);
  const auto latest =
      engine_->project().db().QueryLatestReconstructionByScene(
          scene_row->scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, rows[0].reconstruction_id);
}

// N3 (GO N3) — a mid-chain STAGE FAILURE in the fully-corrected chain: the
// measured chain runs (LC -> PoseGraph -> GTSAM trajectory optimization ->
// apply -> retriangulated v3 -> chain-compaction of the committed revision)
// but the seam's D5 gate REFUSES the refinement at the bundle-adjustment stage.
// The pipeline fails closed exactly as specified: the retriangulated v3 is
// RETAINED (a recovered revision — never erased), the committed revision IS
// compacted, and NO v4 row/artifact or supersede of the valid v3 may occur on
// ANY attempt. Failed runs are retried while recoverable, so multi-attempt
// evidence may accumulate; every retained succeeded revision must still be a
// retriangulated v3, never a v4.
TEST_F(RealGtsamBundleAdjustmentTest,
       N3_StageGateFailureRetainsV3NoV4) {
  const RenderedScene scene = RenderScene(24, 41u, /*noise_px=*/0.4);
  const CommittedGeometry committed = CommittedPointsUnderDrift(scene);
  ASSERT_GT(committed.points.size(), 10u);

  StubOptimizer non_improving(2.0);  // rms_after == rms_before: gate refuses
  std::string traj;
  spatial::engine::ExecutionManifest manifest;
  ASSERT_EQ(RunDrifted("n3_gate_fail", &traj, "trajectory_0", nullptr,
                       committed.points, &scene, /*register_feature_sets=*/true,
                       &non_improving, /*metric_basis_declared=*/true,
                       &manifest),
            "failed");
  ASSERT_EQ(manifest.stages.size(), 2u);
  EXPECT_EQ(manifest.stages[0].status, "succeeded");
  EXPECT_EQ(manifest.stages[1].status, "failed");
  // Measurement evidence was legitimately produced before the gate (the
  // accepted loop closure), but the stage published NO terminal reconstruction
  // artifact: none of its outputs is a canonical reconstruction document.
  const auto is_reconstruction_doc = [&](const std::string& ref) {
    const auto bytes = engine_->project().artifacts().Get(ref);
    if (!bytes.has_value()) return false;
    try {
      const json doc = json::parse(std::string(bytes->begin(), bytes->end()));
      return doc.contains("cameras") && doc.contains("points3D");
    } catch (const std::exception&) {
      return false;
    }
  };
  EXPECT_GE(manifest.stages[1].output_refs.size(), 1u);
  for (const auto& ref : manifest.stages[1].output_refs)
    EXPECT_FALSE(is_reconstruction_doc(ref));

  // The corrected chain up to v3 materialized (retained, valid geometry).
  json v3;
  ASSERT_TRUE(FindRetriangulated(&v3));
  EXPECT_EQ(v3["status"].get<std::string>(), "succeeded");
  EXPECT_EQ(v3["points3D"].size(), committed.points.size());

  // NO bundle-adjusted revision exists under ANY seam: the gate refused, so a
  // v4 was never published on any attempt.
  EXPECT_FALSE(FindBundleAdjusted(nullptr));

  // Persistence topology: the committed identity is chain-compacted, and EVERY
  // succeeded revision is a retained retriangulated v3 (never a v4). The latest
  // succeeded revision is likewise a v3.
  const auto scene_row =
      engine_->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene_row.has_value());
  const auto rows = engine_->project().db().FindReconstructionsByScene(
      scene_row->scene_id);
  ASSERT_GE(rows.size(), 3u);
  const json committed_doc = json::parse(recon_doc_);
  std::map<std::string, std::string> status_by_id;
  for (const auto& row : rows)
    status_by_id[FormatUuid(row.reconstruction_id)] = row.status;
  EXPECT_EQ(
      status_by_id[committed_doc["reconstruction_id"].get<std::string>()],
      "superseded");
  for (const auto& row : rows) {
    if (row.status != "succeeded") continue;
    const json doc = json::parse(row.document_json);
    EXPECT_EQ(doc["provenance"]["backend"]["name"].get<std::string>(),
              "spatial_retriangulator");
  }
  const auto latest =
      engine_->project().db().QueryLatestReconstructionByScene(
          scene_row->scene_id);
  ASSERT_TRUE(latest.has_value());
  const json latest_doc = json::parse(latest->document_json);
  EXPECT_EQ(latest_doc["status"].get<std::string>(), "succeeded");
  EXPECT_EQ(latest_doc["provenance"]["backend"]["name"].get<std::string>(),
            "spatial_retriangulator");
}

}  // namespace

// GTSAM's transitive Boost dependency pulls in boost_test_exec_monitor which// expects this symbol. We define main() ourselves instead of using gtest_main
// to avoid the Boost.Test main() conflicting with GTest's.
int test_main(int, char** const) { return 0; }

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
