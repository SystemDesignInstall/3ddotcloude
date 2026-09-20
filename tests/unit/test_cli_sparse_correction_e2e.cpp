// P3.1 Step 12 — CLI E2E through the SOLE user entry point: the real `spatial`
// executable (`spatial run p3_sparse_correction --input <recon.json> --config
// <json> --project <dir>`).
//
// The fixture seeds the SAME artifacts the production E2E1 fixture seeds (the
// reconstruction CAS document, exact rendered pixels -> feature artifacts, the
// drifted trajectory + metric basis + loop closure). All seeding uses the
// public library APIs (ArtifactStore::Put, WriteFeatureArtifact, Project);
// the CORRECTION itself runs wholly inside the CLI subprocess, which links the
// REAL providers (L2NearestMatcher + EssentialGeometricVerifier +
// GtsamTrajectoryOptimizer + GtsamBundleAdjustmentOptimizer) as the Engine
// composition root. No direct internal pipeline-stage calls from this test.
//
// Assertions:
//   * positive: succeeded manifest (both stages), exit code 0, and the FULL v4
//     chain persisted — committed(applied)/v3 superseded, real-GTSAM v4 the
//     SOLE succeeded/latest revision, committed byte-identical to the input,
//     BA telemetry rms_after<rms_before with byte-identical intrinsics.
//   * negative: a fail-closed config (missing random_seed) yields exit code 1
//     with a "failed" manifest and NO database writes (P12).

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
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

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
#include "engine/pipeline/feature_extraction.h"
#include "engine/workers/child_process.h"

#ifndef SPATIAL_CLI_EXECUTABLE
#error SPATIAL_CLI_EXECUTABLE must be defined by the test build
#endif

namespace {

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
using spatial::core::fs::AtomicWrite;
using spatial::core::fs::Iso8601UtcNow;
using spatial::core::geometry::CameraModel;
using spatial::core::geometry::CameraView;
using spatial::core::geometry::Quaternion;
using spatial::core::geometry::SE3;
using spatial::core::geometry::TriangulateTwoRays;
using spatial::core::geometry::TriangulationAcceptance;
using spatial::core::geometry::TriangulationCandidate;
using spatial::engine::ChildProcess;
using spatial::engine::FeatureExtractionResult;
using spatial::engine::WriteFeatureArtifact;
using spatial::engine::WriteFeatureArtifactInput;
using nlohmann::json;

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Scene fixture helpers — the SAME geometry the production E2E1 fixture uses,
// so the CLI sees an identical, already-proven scene (rendered pixels exact,
// committed poses drifted, metric basis declared).
// ---------------------------------------------------------------------------

Eigen::Matrix3d GroundTruthRotation() {
  const Eigen::AngleAxisd yaw(12.0 * kPi / 180.0, Eigen::Vector3d::UnitY());
  const Eigen::AngleAxisd pitch(-8.0 * kPi / 180.0, Eigen::Vector3d::UnitX());
  const Eigen::AngleAxisd roll(5.0 * kPi / 180.0, Eigen::Vector3d::UnitZ());
  return (yaw * pitch * roll).toRotationMatrix();
}

Eigen::Vector3d GroundTruthTranslation() { return {0.6, -0.2, 0.15}; }

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

struct RenderedPair {
  std::vector<std::pair<double, double>> src;
  std::vector<std::pair<double, double>> tgt;
};

struct RenderedScene {
  std::vector<Eigen::Vector3d> X;  // true world points (source camera at origin)
  RenderedPair pair;               // pixels in src / tgt, aligned with X
};

RenderedScene RenderScene(int n_points, std::uint32_t seed,
                          double noise_px) {
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

struct CommittedGeometry {
  std::vector<ReconPoint3D> points;  // aligned one-to-one with truth
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

double MeanDistanceToTruth(const CommittedGeometry& committed, const json& doc) {
  double sum = 0.0;
  for (std::size_t i = 0; i < committed.points.size(); ++i) {
    const Eigen::Vector3d truth = committed.truth[i];
    const Eigen::Vector3d pos(doc["points3D"][i]["xyz"][0].get<double>(),
                              doc["points3D"][i]["xyz"][1].get<double>(),
                              doc["points3D"][i]["xyz"][2].get<double>());
    sum += (pos - truth).norm();
  }
  return sum / static_cast<double>(committed.points.size());
}

// ---------------------------------------------------------------------------
// Child-process CLI runner (same discipline as test_cli_import.cpp).
// ---------------------------------------------------------------------------

struct CliRun {
  int exit_code = -1;
  std::string stdout_text;
};

CliRun RunCli(const std::vector<std::string>& argv) {
  std::string error;
  auto child = ChildProcess::Spawn(argv, error);
  if (!child) {
    ADD_FAILURE() << "cli spawn failed: " << error;
    return {-1, ""};
  }
  CliRun run;
  std::vector<char> buf(16384);
  while (true) {
    std::size_t n = 0;
    bool eof = false;
    std::string err;
    if (child->ReadStdout(buf.data(), buf.size(), 20000, n, eof, err)) {
      run.stdout_text.append(buf.data(), n);
    } else {
      run.stdout_text.append(buf.data(), n);
      if (eof) break;
      ADD_FAILURE() << "cli stdout read failed: " << err;
      child->Terminate();
      break;
    }
  }
  run.exit_code = child->Wait();
  return run;
}

// The CLI prints ONLY the manifest JSON on stdout, but the GTSAM backend emits
// LM diagnostics ("Partial Cholesky on HessianFactor failed..." — normal
// damping fallback) that can precede it. Parse the manifest object itself.
json ParseStdoutJson(const std::string& text) {
  const std::size_t start = text.find_first_of('{');
  if (start == std::string::npos) {
    throw std::runtime_error("CLI stdout contains no JSON manifest");
  }
  return json::parse(text.substr(start));
}

// ---------------------------------------------------------------------------
// CLI E2E fixture.
// ---------------------------------------------------------------------------

class CliSparseCorrectionE2eTest : public ::testing::Test {
 protected:
  struct Seed {
    std::string recon_doc;
    std::string cfg;  // full corrected-chain run config (inline --config JSON)
    std::string trajectory_id;
    std::string src_frame;
    std::string tgt_frame;
    CommittedGeometry committed;
    std::filesystem::path input_file;
  };

  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_cli_e2e_p3_" + std::to_string(std::time(nullptr)) +
             "_" + std::to_string(rand()));
    std::filesystem::create_directories(root_);
    project_path_ = root_ / "demo.spx";
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "cli-e2e-p3";
    info.created_at = Iso8601UtcNow();
    project_id_ = info.uuid;
    project_ = std::make_unique<Project>(Project::Create(project_path_, info));
  }

  void TearDown() override {
    project_.reset();
    evidence_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Seeds the project the CLI will run against: reconstruction document in the
  // CAS + on disk (--input), the two feature artifacts the loop-closure needs,
  // and the FULL corrected-chain config. `with_seed`=false drops random_seed so
  // stage-2 fails closed BEFORE any DB write (P12).
  Seed SeedProject(const std::string& scene_name, bool with_seed) {
    const RenderedScene scene = RenderScene(36, 7u, /*noise_px=*/0.4);
    Seed seed;
    seed.committed = CommittedPointsUnderDrift(scene);
    EXPECT_GT(seed.committed.points.size(), 10u);

    const std::string src_frame = FormatUuid(GenerateUuid());
    const std::string tgt_frame = FormatUuid(GenerateUuid());
    Reconstruction recon;
    recon.reconstruction_id = FormatUuid(GenerateUuid());
    recon.scene_id = "22222222-2222-4222-8222-222222222222";
    recon.session_ids = {"33333333-3333-4333-8333-333333333333"};
    recon.coordinate_frame = "trajectory_0";
    recon.status = "succeeded";
    recon.created_at_ns = 100;
    recon.provenance.backend.name = "colmap";
    recon.provenance.backend.version = "v2";
    recon.provenance.backend.adapter_version = "test";
    recon.provenance.configuration_hash =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    recon.cameras.push_back(PinholeCamera(1));
    recon.cameras.push_back(PinholeCamera(2));
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
    recon.points3D = seed.committed.points;  // copy: Seed keeps them for error measurement
    seed.recon_doc = ReconstructionToJson(recon);
    const std::vector<std::uint8_t> doc_bytes(seed.recon_doc.begin(),
                                              seed.recon_doc.end());
    seed.input_file = root_ / ("input-" + scene_name + ".json");
    AtomicWrite(seed.input_file, doc_bytes);

    auto& db = project_->db();
    auto& store = project_->artifacts();
    ArtifactManifest doc_manifest;
    doc_manifest.artifact_uuid = GenerateUuid();
    doc_manifest.type = "reconstruction";
    doc_manifest.schema_version = 2;
    doc_manifest.producer = {"spatial-platform", "0.1.0", "test"};
    doc_manifest.creation_timestamp = Iso8601UtcNow();
    doc_manifest.file_size = static_cast<std::int64_t>(doc_bytes.size());
    store.Put(doc_bytes, doc_manifest);

    auto put_features = [&](const Uuid& frame_id,
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
      const FeatureExtractionResult result =
          WriteFeatureArtifact(store, db, input);
      return FormatUuid(result.artifact_uuid);
    };
    const std::string src_art = put_features(ParseUuid(src_frame), scene.pair.src);
    const std::string tgt_art = put_features(ParseUuid(tgt_frame), scene.pair.tgt);

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
          {"position_xyz", {drifted_t.x(), drifted_t.y(), drifted_t.z()}},
          {"rotation_xyzw",
           {drifted_r[0], drifted_r[1], drifted_r[2], drifted_r[3]}}}});
    const json basis = {{"declared", true},
                        {"type", "odometry_scale"},
                        {"source", "trajectory"},
                        {"scale_calibration_ref", "calib-ref-1"},
                        {"provenance",
                         {{"configuration_hash",
                           "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
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
                {"scene_name", scene_name},
                {"random_seed", "pinned-step12"}};
    cfg.update(lc);
    if (!with_seed) cfg.erase("random_seed");
    seed.cfg = cfg.dump();
    seed.trajectory_id = traj;
    seed.src_frame = src_frame;
    seed.tgt_frame = tgt_frame;
    return seed;
  }

  CliRun RunCorrection(const Seed& seed) {
    const std::vector<std::string> argv = {kCliExe, "run",
                                           "p3_sparse_correction",
                                           "--project", project_path_.string(),
                                           "--input", seed.input_file.string(),
                                           "--config", seed.cfg};
    return RunCli(argv);
  }

  std::filesystem::path root_;
  std::filesystem::path project_path_;
  Uuid project_id_{};
  std::unique_ptr<Project> project_;
  std::unique_ptr<Project> evidence_;

 private:
  static const std::string kCliExe;
};

const std::string CliSparseCorrectionE2eTest::kCliExe = SPATIAL_CLI_EXECUTABLE;

TEST_F(CliSparseCorrectionE2eTest, RealCliCorrectsAndPersistsFullV4Chain) {
  const Seed seed = SeedProject("e2e_cli_pos", /*with_seed=*/true);

  // The CLI is the sole actor here: seeding releases the project lock, then the
  // spawned executable runs the whole corrected chain itself.
  const CliRun run = RunCorrection(seed);
  ASSERT_EQ(run.exit_code, 0) << "stdout:\n" << run.stdout_text;

  const json manifest = ParseStdoutJson(run.stdout_text);
  EXPECT_EQ(manifest["status"].get<std::string>(), "succeeded");
  ASSERT_EQ(manifest["stages"].size(), 2u);
  EXPECT_EQ(manifest["stages"][0]["status"].get<std::string>(), "succeeded");
  EXPECT_EQ(manifest["stages"][1]["status"].get<std::string>(), "succeeded");

  // Reopen the project (the CLI has exited) and walk the persisted evidence.
  evidence_ = std::make_unique<Project>(Project::Open(project_path_));
  auto& db = evidence_->db();
  const auto scene_row = db.FindSceneByProject(project_id_);
  ASSERT_TRUE(scene_row.has_value());
  const auto rows = db.FindReconstructionsByScene(scene_row->scene_id);
  ASSERT_EQ(rows.size(), 4u);

  std::map<std::string, json> doc_by_backend;  // one revision per row
  std::map<std::string, std::string> status_by_backend;
  for (const auto& row : rows) {
    const json doc = json::parse(row.document_json);
    const std::string backend =
        doc["provenance"]["backend"]["name"].get<std::string>();
    doc_by_backend[backend] = doc;
    status_by_backend[backend] = row.status;
  }

  // committed (colmap) superseded + byte-identical to the submitted document.
  EXPECT_EQ(status_by_backend["colmap"], "superseded");
  EXPECT_EQ(doc_by_backend["colmap"].dump(), json::parse(seed.recon_doc).dump());
  // applied / retriangulated / real-GTSAM BA — the production chain order.
  EXPECT_EQ(status_by_backend["spatial_optimizer"], "superseded");
  EXPECT_EQ(status_by_backend["spatial_retriangulator"], "superseded");
  EXPECT_EQ(status_by_backend["spatial_gtsam_bundle_adjuster"], "succeeded");
  int succeeded = 0;
  for (const auto& entry : status_by_backend)
    if (entry.second == "succeeded") ++succeeded;
  EXPECT_EQ(succeeded, 1);

  const json& applied = doc_by_backend["spatial_optimizer"];
  const json& v3 = doc_by_backend["spatial_retriangulator"];
  const json& v4 = doc_by_backend["spatial_gtsam_bundle_adjuster"];
  EXPECT_EQ(v3["images"], applied["images"]);
  EXPECT_EQ(v3["cameras"], applied["cameras"]);
  EXPECT_EQ(v4["cameras"], v3["cameras"]);
  EXPECT_EQ(v4["created_at_ns"].get<std::int64_t>(),
            v3["created_at_ns"].get<std::int64_t>() + 100);
  const json ba_telemetry = json::parse(
      v4["provenance"]["backend_specific_json"].get<std::string>());
  EXPECT_EQ(ba_telemetry["optimizer"].get<std::string>(),
            "LevenbergMarquardt");
  EXPECT_TRUE(ba_telemetry["converged"].get<bool>());
  EXPECT_GT(ba_telemetry["rms_before"].get<double>(), 0.0);
  EXPECT_LT(ba_telemetry["rms_after"].get<double>(),
            ba_telemetry["rms_before"].get<double>());

  // Geometry really changed through the CLI: retriangulated v3 and the v4
  // refinement both land closer to the rendered truth than the drifted commit.
  const double committed_err =
      MeanDistanceToTruth(seed.committed, doc_by_backend["colmap"]);
  const double v3_err = MeanDistanceToTruth(seed.committed, v3);
  EXPECT_GT(committed_err, 0.05);
  EXPECT_GT(v3_err, 1e-6);
  EXPECT_LT(v3_err, committed_err);

  // GTSAM trajectory optimization evidence: the node moved off the drift.
  const auto opt_rows = db.FindOptimizationResultsByTrajectory(
      ParseUuid(seed.trajectory_id));
  ASSERT_EQ(opt_rows.size(), 1u);
  EXPECT_EQ(opt_rows[0].status, "converged");
  EXPECT_GT(opt_rows[0].initial_error, 0.0);
  EXPECT_LT(opt_rows[0].final_error, opt_rows[0].initial_error);
  const auto opt_artifacts = db.FindArtifactsByType("optimization_result");
  ASSERT_EQ(opt_artifacts.size(), 1u);
  const auto opt_bytes = evidence_->artifacts().Get(opt_artifacts.front().content_hash);
  ASSERT_TRUE(opt_bytes.has_value());
  const json opt = json::parse(std::string(opt_bytes->begin(), opt_bytes->end()));
  const Eigen::Vector3d drifted = DriftedTargetTranslation();
  bool found = false;
  for (const auto& n : opt["nodes"]) {
    if (n["frame_id"].get<std::string>() != seed.tgt_frame) continue;
    const Eigen::Vector3d p_tgt(n["position_xyz"][0].get<double>(),
                                n["position_xyz"][1].get<double>(),
                                n["position_xyz"][2].get<double>());
    EXPECT_GT((p_tgt - drifted).norm(), 0.05);
    found = true;
  }
  EXPECT_TRUE(found);

  // latest revision == the real-GTSAM v4.
  const auto latest =
      db.QueryLatestReconstructionByScene(scene_row->scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(FormatUuid(latest->reconstruction_id),
            v4["reconstruction_id"].get<std::string>());
}

TEST_F(CliSparseCorrectionE2eTest, FailedManifestReturnsNonZeroAndWritesNothing) {
  // missing random_seed (D6) => stage-2 fail-closed BEFORE any DB write (P12).
  const Seed seed = SeedProject("e2e_cli_neg", /*with_seed=*/false);

  const CliRun run = RunCorrection(seed);
  EXPECT_EQ(run.exit_code, 1);

  const json manifest = ParseStdoutJson(run.stdout_text);
  EXPECT_EQ(manifest["status"].get<std::string>(), "failed");
  ASSERT_EQ(manifest["stages"].size(), 2u);
  EXPECT_EQ(manifest["stages"][0]["status"].get<std::string>(), "succeeded");
  EXPECT_EQ(manifest["stages"][1]["status"].get<std::string>(), "failed");

  // P12: the failed validation happened before any database write — no scene,
  // no revision rows, nothing superseded, nothing corrected.
  evidence_ = std::make_unique<Project>(Project::Open(project_path_));
  EXPECT_FALSE(evidence_->db().FindSceneByProject(project_id_).has_value());
}

}  // namespace