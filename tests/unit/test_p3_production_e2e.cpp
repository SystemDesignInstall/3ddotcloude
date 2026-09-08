// P3-Production-E2E — golden positive + negatives N1-N6 (§6.2/§5).
//
// The production correction chain entered through the REAL engine surface
// (Engine::RunPipeline) and the host-runner orchestration roles:
//
//   1. WORKER COMPUTE  : Engine::RunPipeline("sparse_reconstruction") dispatch
//                        to the colmap_worker subprocess (ProcessExecutor +
//                        probe shim, CI-gated). Workers touch ONLY the CAS.
//   2. HOST COMMIT     : CommitWorkerReconstructionArtifact (sparse_correction_
//                        orchestrator.h) materializes the worker CAS payload
//                        as a SUCCEEDED revision — the only DB writer on the
//                        path (worker boundary). Fail-closed on malformed /
//                        non-succeeded payloads (N3).
//   3. STAGE-6 (D5/P14): BundleAdjustmentOptimizePipeline over the project DB:
//                        v_n+1 insert + v_n supersede inside ONE
//                        MetadataDb::WriteTransaction (§6/P12 atomic). The D5
//                        gate refuses non-improving seam output (N1).
//   4. STAGE-5 (INV-3) : LoopClosureOptimizePipeline over the project DB +
//                        CAS: metric-ineligible closures persist for audit but
//                        manufacture NO pose graph / optimization (N4).
//
// Golden assertions (§6.2): v2(v1 provenance)->v3->v4 revision chain with the
// superseded/succeeded lifecycle preserved, QueryLatest returning the sole
// succeeded v4, created_at strictly increasing (P14 ordering), D6
// determinism (ADR-020 worker replay from the task cache; the pinned seed
// reaching both seam calls identically), and CAS content persistence for the
// produced revision documents (§3.3/§8). Negatives N1-N6 assert every fail-
// closed path writes NOTHING (P12 no-partial-results).

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_manifest.h"
#include "core/errors/project_error.h"
#include "core/geometry/reconstruction_optimizer.h"
#include "core/project/project.h"
#include "core/reconstruction/reconstruction.h"
#include "core/reconstruction/reconstruction_json.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/bundle_adjustment_optimize_pipeline.h"
#include "engine/pipeline/loop_closure_optimize_pipeline.h"
#include "engine/pipeline/pipeline_definition.h"
#include "engine/pipeline/sparse_correction_orchestrator.h"
#include "engine/workers/process_executor.h"

#ifndef SPATIAL_COLMAP_WORKER_EXECUTABLE
#error SPATIAL_COLMAP_WORKER_EXECUTABLE must be defined by the test build
#endif
#ifndef SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE
#error SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE must be defined by the test build
#endif

namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::LoopClosure;
using spatial::core::MetadataDb;
using spatial::core::ParseUuid;
using spatial::core::Project;
using spatial::core::ProjectInfo;
using spatial::core::ReconImage;
using spatial::core::Reconstruction;
using spatial::core::ReconstructionFromJson;
using spatial::core::ReconstructionRow;
using spatial::core::ReconstructionToJson;
using spatial::core::SceneRow;
using spatial::core::Trajectory;
using spatial::core::TrajectoryPoseNode;
using spatial::core::Uuid;
using spatial::core::ValidationError;
using spatial::core::fs::Iso8601UtcNow;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::BundleAdjustmentTrace;
using spatial::core::geometry::ReconstructionOptimizer;
using spatial::engine::BundleAdjustmentOptimizePipeline;
using spatial::engine::BundleAdjustmentOptimizePipelineInput;
using spatial::engine::Engine;
using spatial::engine::PipelineRegistry;
using spatial::engine::ProcessExecutor;
using nlohmann::json;

// Canonical (schema-valid) reconstruction document with one camera + one image,
// enough for the schema round-trip and the DB revision path.
Reconstruction MakeReconstruction(const std::string& id,
                                  const std::string& status,
                                  std::int64_t created_at_ns) {
  Reconstruction rec;
  rec.reconstruction_id = id;
  rec.scene_id = "22222222-2222-4222-8222-222222222222";
  rec.session_ids = {"33333333-3333-4333-8333-333333333333"};
  rec.coordinate_frame = "reconstruction_0";
  rec.status = status;
  rec.created_at_ns = created_at_ns;
  rec.provenance.backend.name = "colmap";
  rec.provenance.backend.version = "v2";
  rec.provenance.configuration_hash =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  spatial::core::ReconCamera cam;
  cam.camera_id = 1;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsic_model = "pinhole";
  cam.fx = 500.0;
  cam.fy = 500.0;
  cam.cx = 320.0;
  cam.cy = 240.0;
  cam.distortion_model = "none";
  rec.cameras.push_back(cam);

  ReconImage image;
  image.image_id = 1;
  image.camera_id = 1;
  image.frame_id = "44444444-4444-4444-8444-444444444441";
  image.name = "frame_0.jpg";
  image.pose.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  image.pose.translation_xyz = {1.0, 2.0, 3.0};
  image.detected = true;
  rec.images.push_back(image);
  return rec;
}

// Seeds `rec` into `db` as a reconstruction row; returns the freshly parsed
// canonical copy the host-runner stages read back.
Reconstruction SeedSucceeded(MetadataDb& db, const Uuid& scene_id,
                             const Reconstruction& rec) {
  ReconstructionRow row;
  row.reconstruction_id = ParseUuid(rec.reconstruction_id);
  row.scene_id = scene_id;
  row.coordinate_frame = rec.coordinate_frame;
  row.status = rec.status;
  row.created_at_ns = rec.created_at_ns;
  row.document_json = ReconstructionToJson(rec);
  db.AddReconstruction(row);
  return ReconstructionFromJson(row.document_json);
}

// Deterministic stage-6 seam stand-in (fake optics on the seam boundary; the
// seam never touches a backend). The produced v_n+1 carries created_at_ns =
// source.created_at_ns + 100 so repeated passes keep the revision clock
// strictly increasing (P14 ordering is observable).
class StubOptimizer : public ReconstructionOptimizer {
 public:
  explicit StubOptimizer(double rms_after) : rms_after_(rms_after) {}

  BundleAdjustmentResult optimize(const BundleAdjustmentInput& input) override {
    seen_seeds_.push_back(input.random_seed ? *input.random_seed : "");
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

  double rms_after_;
  std::vector<std::string> seen_seeds_;
};

TrajectoryPoseNode MakeNode(std::int64_t seq, const std::string& frame_id,
                            double x, double y, double z) {
  TrajectoryPoseNode n;
  n.frame_id = frame_id;
  n.sequence_index = seq;
  n.position_xyz = {x, y, z};
  n.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  return n;
}

LoopClosure MakeAcceptedClosure(const std::vector<TrajectoryPoseNode>& nodes,
                                const std::string& trajectory_id,
                                const std::string& closure_id) {
  LoopClosure lc;
  lc.closure_id = closure_id;
  lc.trajectory_id = trajectory_id;
  lc.candidate_id = "00000000-0000-0000-0000-0000000000CA";
  lc.source_frame_id = nodes[1].frame_id;
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

class P3ProductionE2eTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_p3_prod_e2e_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "p3-prod-e2e";
    info.created_at = Iso8601UtcNow();
    project_id_ = info.uuid;
    project_ = std::make_unique<Project>(
        Project::Create(root_ / "demo.spx", info));
  }

  void TearDown() override {
    project_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  static void RegisterSparseReconstruction(PipelineRegistry& registry) {
    spatial::engine::PipelineDefinition def;
    def.id = "sparse_reconstruction";
    def.version = "0.1.0";
    def.git_commit = "test";
    def.stages = {
        {"reconstruct", "sparse_reconstruction", "sparse_reconstruction",
         {"image"}, {"reconstruction"}},
    };
    registry.Register(std::move(def));
  }

  std::string PutImage(const std::string& content) {
    const std::vector<std::uint8_t> bytes(content.begin(), content.end());
    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = "image";
    manifest.producer = {"spatial-platform", "0.1.0", "test"};
    manifest.creation_timestamp = Iso8601UtcNow();
    manifest.file_size = static_cast<std::int64_t>(bytes.size());
    return project_->artifacts().Put(bytes, manifest).content_hash;
  }

  std::string PutReconstructionPayload(const std::vector<std::uint8_t>& bytes) {
    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = "reconstruction";
    manifest.producer = {"spatial-platform", "0.1.0", "test"};
    manifest.creation_timestamp = Iso8601UtcNow();
    manifest.file_size = static_cast<std::int64_t>(bytes.size());
    return project_->artifacts().Put(bytes, manifest).content_hash;
  }

  std::unique_ptr<ProcessExecutor> MakeExecutor() {
    return std::make_unique<ProcessExecutor>(
        spatial::engine::ResourceProfile{},
        std::vector<std::string>{SPATIAL_COLMAP_WORKER_EXECUTABLE,
                                 SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE},
        "", 5000, &project_->artifacts());
  }

  std::filesystem::path root_;
  std::unique_ptr<Project> project_;
  Uuid project_id_{};
};

// ============================ GOLDEN POSITIVE (§6.2) =========================

TEST_F(P3ProductionE2eTest, GoldenCorrectionChainCommitsSupersedesAndReplays) {
  const std::string image1 = PutImage("prod-e2e-image-1");
  const std::string image2 = PutImage("prod-e2e-image-2");

  Engine engine(std::move(*project_), MakeExecutor());
  project_.reset();
  RegisterSparseReconstruction(engine.registry());

  // 1. WORKER COMPUTE: sparse reconstruction over the CAS through the real
  //    Engine::RunPipeline surface.
  const auto first =
      engine.RunPipeline("sparse_reconstruction", {image1, image2}, "{}");
  ASSERT_EQ(first.status, "succeeded");
  ASSERT_EQ(first.stages.size(), 1u);
  EXPECT_EQ(first.stages[0].status, "succeeded");
  EXPECT_EQ(first.stages[0].implementation, "process");
  ASSERT_EQ(first.stages[0].output_refs.size(), 1u);
  const std::string v2_hash = first.stages[0].output_refs.front();
  EXPECT_TRUE(engine.project().artifacts().Has(v2_hash));

  // The v2 artifact is a canonical reconstruction (CAS content identity +
  // manifest lineage back to the two image inputs).
  const auto indexed = engine.project().db().FindArtifactByHash(v2_hash);
  ASSERT_TRUE(indexed.has_value());
  const auto manifest = engine.project().artifacts().ReadManifest(indexed->artifact_id);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->type, "reconstruction");
  EXPECT_EQ(manifest->schema_version, 2);
  ASSERT_EQ(manifest->input_artifact_hashes.size(), 2u);

  auto payload = engine.project().artifacts().Get(v2_hash);
  ASSERT_TRUE(payload.has_value());
  const json doc =
      json::parse(std::string(payload->begin(), payload->end()));
  EXPECT_EQ(doc["schema_version"].get<int>(), 2);
  EXPECT_TRUE(doc.contains("reconstruction_id"));
  EXPECT_EQ(doc["status"].get<std::string>(), "succeeded");

  // D6/ADR-020: identical inputs + config -> identical pipeline identity and
  // output, served from the task cache (no worker re-run).
  const auto second =
      engine.RunPipeline("sparse_reconstruction", {image1, image2}, "{}");
  EXPECT_EQ(second.pipeline_hash, first.pipeline_hash);
  EXPECT_EQ(second.stages[0].output_refs, first.stages[0].output_refs);
  EXPECT_TRUE(second.stages[0].cache_hit);

  // 2. HOST COMMIT: materialize the worker CAS artifact as a SUCCEEDED
  //    revision (workers never touch the MetadataDb).
  auto& db = engine.project().db();
  const SceneRow scene = db.FindOrCreateScene(project_id_, "p3_prod_e2e_scene",
                                              "{}", 2000);
  Reconstruction v2 = spatial::engine::CommitWorkerReconstructionArtifact(
      db, scene.scene_id, *payload, /*created_at_ns=*/100);
  EXPECT_FALSE(v2.reconstruction_id.empty());
  // The revision clock is the HOST-ASSIGNED value (revision lifecycle), not
  // the worker document's own field; feed the committed source with it so the
  // seam can extend the clock (P14 ordering is observable in the rows).
  v2.created_at_ns = 100;
  {
    const auto rows = db.FindReconstructionsByScene(scene.scene_id);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].status, "succeeded");
  }

  // 3. STAGE-6 BA pass 1: v2 -> v3 (gate 1.0 < 0.9*2.0 strictly passes).
  StubOptimizer seam1(1.0);
  BundleAdjustmentOptimizePipelineInput in1;
  in1.source_v3 = &v2;
  in1.optimizer = &seam1;
  in1.db = &db;
  in1.scene_id = scene.scene_id;
  in1.random_seed = "pinned-p3-prod-e2e";
  const auto pass1 = BundleAdjustmentOptimizePipeline(in1);
  ASSERT_TRUE(pass1.ran);
  EXPECT_TRUE(pass1.gate_passed);
  EXPECT_TRUE(pass1.inserted_v4);
  EXPECT_TRUE(pass1.superseded_v3);
  ASSERT_TRUE(pass1.v4.has_value());
  ASSERT_TRUE(pass1.trace.has_value());

  // STAGE-6 BA pass 2 on the v3 -> v4: the v3 is the sole succeeded revision.
  const auto latest_v3 = db.QueryLatestReconstructionByScene(scene.scene_id);
  ASSERT_TRUE(latest_v3.has_value());
  EXPECT_EQ(latest_v3->reconstruction_id, ParseUuid(pass1.v4->reconstruction_id));
  EXPECT_EQ(latest_v3->status, "succeeded");
  const Reconstruction v3 = ReconstructionFromJson(latest_v3->document_json);

  StubOptimizer seam2(0.8);
  BundleAdjustmentOptimizePipelineInput in2;
  in2.source_v3 = &v3;
  in2.optimizer = &seam2;
  in2.db = &db;
  in2.scene_id = scene.scene_id;
  in2.random_seed = "pinned-p3-prod-e2e";
  const auto pass2 = BundleAdjustmentOptimizePipeline(in2);
  ASSERT_TRUE(pass2.ran);
  EXPECT_TRUE(pass2.gate_passed);
  EXPECT_TRUE(pass2.inserted_v4);
  EXPECT_TRUE(pass2.superseded_v3);

  // P14 revision lifecycle + ordering: v2 and v3 superseded, v4 the ONLY
  // succeeded revision (and therefore LATEST), timestamps strictly increasing.
  const auto latest = db.QueryLatestReconstructionByScene(scene.scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->status, "succeeded");
  EXPECT_EQ(latest->reconstruction_id, ParseUuid(pass2.v4->reconstruction_id));
  const auto all = db.FindReconstructionsByScene(scene.scene_id);
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(all[0].status, "superseded");
  EXPECT_EQ(all[1].status, "superseded");
  EXPECT_EQ(all[2].status, "succeeded");
  EXPECT_LT(all[0].created_at_ns, all[1].created_at_ns);
  EXPECT_LT(all[1].created_at_ns, all[2].created_at_ns);
  EXPECT_NE(all[0].reconstruction_id, all[1].reconstruction_id);
  EXPECT_NE(all[1].reconstruction_id, all[2].reconstruction_id);

  // D6: the pinned seed reached BOTH seam invocations identically.
  ASSERT_EQ(seam1.seen_seeds_.size(), 1u);
  EXPECT_EQ(seam1.seen_seeds_[0], "pinned-p3-prod-e2e");
  ASSERT_EQ(seam2.seen_seeds_.size(), 1u);
  EXPECT_EQ(seam2.seen_seeds_[0], "pinned-p3-prod-e2e");

  // §3.3/§8: the superseded-revision CAS payload is RETENTED (never deleted);
  // the canonical v4 document is persisted on the revision row.
  EXPECT_TRUE(engine.project().artifacts().Has(v2_hash));
  const Reconstruction from_doc = ReconstructionFromJson(all[2].document_json);
  EXPECT_EQ(from_doc.reconstruction_id, FormatUuid(all[2].reconstruction_id));
  EXPECT_EQ(from_doc.status, "succeeded");
}

// ============================ NEGATIVES N1-N6 (§5) ===========================

// N1: the D5 gate refuses a non-improving seam result — NO v4 insert, NO
// supercede; the v3 stays the only succeeded revision.
TEST_F(P3ProductionE2eTest, N1NonImprovingPassFailsGateWithoutWrites) {
  auto& db = project_->db();
  const SceneRow scene = db.FindOrCreateScene(project_id_, "p3_prod_e2e_scene",
                                              "{}", 2000);
  const Reconstruction v2 = SeedSucceeded(
      db, scene.scene_id,
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100));

  StubOptimizer improving(1.0);
  BundleAdjustmentOptimizePipelineInput in1;
  in1.source_v3 = &v2;
  in1.optimizer = &improving;
  in1.db = &db;
  in1.scene_id = scene.scene_id;
  in1.random_seed = "pinned-p3-prod-e2e";
  const auto pass1 = BundleAdjustmentOptimizePipeline(in1);
  ASSERT_TRUE(pass1.gate_passed);
  ASSERT_TRUE(pass1.v4.has_value());

  const auto latest_v3 = db.QueryLatestReconstructionByScene(scene.scene_id);
  ASSERT_TRUE(latest_v3.has_value());
  const Reconstruction v3 = ReconstructionFromJson(latest_v3->document_json);

  // Mirror-image failure: rms_after == rms_before == 2.0 (NOT < 0.9*2.0).
  StubOptimizer non_improving(2.0);
  BundleAdjustmentOptimizePipelineInput in2;
  in2.source_v3 = &v3;
  in2.optimizer = &non_improving;
  in2.db = &db;
  in2.scene_id = scene.scene_id;
  in2.random_seed = "pinned-p3-prod-e2e";
  const auto pass2 = BundleAdjustmentOptimizePipeline(in2);
  EXPECT_TRUE(pass2.ran);
  EXPECT_FALSE(pass2.gate_passed);
  EXPECT_FALSE(pass2.inserted_v4);
  EXPECT_FALSE(pass2.superseded_v3);

  // No row was added after the v3, and the v3 is still the succeeded latest.
  const auto all = db.FindReconstructionsByScene(scene.scene_id);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].status, "superseded");  // the v2
  EXPECT_EQ(all[1].status, "succeeded");   // the v3, the latest
  const auto latest = db.QueryLatestReconstructionByScene(scene.scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, ParseUuid(v3.reconstruction_id));
}

// N2: a worker input whose CAS reference is missing fails closed on the host
// in the manifest — and nothing is committed to the revision history.
TEST_F(P3ProductionE2eTest, N2MissingWorkerInputFailsClosedAndNothingCommitted) {
  const std::string bogus(64, 'd');  // never in the CAS
  Engine engine(std::move(*project_), MakeExecutor());
  project_.reset();
  RegisterSparseReconstruction(engine.registry());
  const SceneRow scene = engine.project().db().FindOrCreateScene(
      project_id_, "p3_prod_e2e_scene", "{}", 2000);

  const auto manifest =
      engine.RunPipeline("sparse_reconstruction", {bogus}, "{}");
  EXPECT_EQ(manifest.status, "failed");
  ASSERT_EQ(manifest.stages.size(), 1u);
  EXPECT_EQ(manifest.stages[0].status, "failed");
  EXPECT_FALSE(engine.project().artifacts().Has(
      manifest.stages[0].output_refs.front()));

  EXPECT_TRUE(engine.project().db().FindReconstructionsByScene(scene.scene_id)
                  .empty());
}

// N3: the host-runner COMMIT fails closed on a CAS payload that is not a
// canonical, provisional-succeeded reconstruction document — nothing is
// written.
TEST_F(P3ProductionE2eTest, N3CorruptPayloadFailsClosedBeforeAnyCommit) {
  auto& db = project_->db();
  const SceneRow scene = db.FindOrCreateScene(project_id_, "p3_prod_e2e_scene",
                                              "{}", 2000);

  // Not a canonical document at all.
  const std::string garbage = "this is not a reconstruction document {{{";
  const std::vector<std::uint8_t> garbage_bytes(garbage.begin(), garbage.end());
  PutReconstructionPayload(garbage_bytes);
  EXPECT_THROW(spatial::engine::CommitWorkerReconstructionArtifact(
                   db, scene.scene_id, garbage_bytes, 100),
               ValidationError);

  // Parseable but NOT provisional-succeeded.
  const Reconstruction non_succeeded =
      MakeReconstruction(FormatUuid(GenerateUuid()), "reconstructing", 100);
  const std::string non_succeeded_doc = ReconstructionToJson(non_succeeded);
  PutReconstructionPayload(
      std::vector<std::uint8_t>(non_succeeded_doc.begin(),
                                non_succeeded_doc.end()));
  EXPECT_THROW(spatial::engine::CommitWorkerReconstructionArtifact(
                   db, scene.scene_id,
                   std::vector<std::uint8_t>(non_succeeded_doc.begin(),
                                             non_succeeded_doc.end()),
                   100),
               ValidationError);

  EXPECT_TRUE(db.FindReconstructionsByScene(scene.scene_id).empty());
}

// N4: a metric-ineligible closure (INV-3: undeclared metric basis) is still
// persisted for audit but manufactures NO pose graph, NO optimization result.
TEST_F(P3ProductionE2eTest, N4MetricIneligibleClosurePersistsWithoutOptimization) {
  auto& db = project_->db();
  const SceneRow scene = db.FindOrCreateScene(project_id_, "p3_prod_e2e_scene",
                                              "{}", 2000);

  Trajectory trajectory;
  trajectory.trajectory_id = "00000000-0000-0000-0000-0000000000E1";
  trajectory.scene_id = FormatUuid(scene.scene_id);
  trajectory.session_id = "00000000-0000-0000-0000-0000000000E2";
  trajectory.kind = "odometry";
  trajectory.status = "building";
  trajectory.node_count = 2;
  trajectory.created_at_ns = 100;
  trajectory.metric_basis = spatial::core::MetricBasis{};  // undeclared

  const std::vector<TrajectoryPoseNode> nodes = {
      MakeNode(0, "00000000-0000-0000-0000-0000000000F0", 0.0, 0.0, 0.0),
      MakeNode(1, "00000000-0000-0000-0000-0000000000F1", 0.0, 0.31, 0.33)};

  spatial::engine::LoopClosureOptimizePipelineInput in;
  in.trajectory = &trajectory;
  in.trajectory_nodes = &nodes;
  in.closure = MakeAcceptedClosure(
      nodes, trajectory.trajectory_id, "00000000-0000-0000-0000-0000000000C3");
  in.metric_basis = spatial::core::MetricBasis{};
  in.configuration_hash = "cfg-hash";
  in.optimizer = nullptr;
  in.db = &db;
  in.store = &project_->artifacts();

  const auto out = spatial::engine::LoopClosureOptimizePipeline(in);
  EXPECT_FALSE(out.ran);
  EXPECT_FALSE(out.metric_eligible);
  EXPECT_FALSE(out.optimization.has_value());

  // Closure persisted for audit (D2 spatial_separation_m filled); no pose
  // graph and no optimization were manufactured.
  const auto closures = db.FindLoopClosuresByTrajectory(
      ParseUuid(trajectory.trajectory_id));
  ASSERT_EQ(closures.size(), 1u);
  EXPECT_NEAR(closures[0].spatial_separation_m, out.spatial_separation_m, 1e-9);
  const auto graphs = db.FindPoseGraphsByTrajectory(
      ParseUuid(trajectory.trajectory_id));
  EXPECT_TRUE(graphs.empty());
  const auto opts = db.FindOptimizationResultsByTrajectory(
      ParseUuid(trajectory.trajectory_id));
  EXPECT_TRUE(opts.empty());
}

// N5: an unpinned random seed fails closed (typed ValidationError) BEFORE any
// database write.
TEST_F(P3ProductionE2eTest, N5UnpinnedSeedFailsClosedBeforeAnyWrite) {
  auto& db = project_->db();
  const SceneRow scene = db.FindOrCreateScene(project_id_, "p3_prod_e2e_scene",
                                              "{}", 2000);
  const Reconstruction v2 = SeedSucceeded(
      db, scene.scene_id,
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100));

  StubOptimizer seam(1.0);
  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v2;
  in.optimizer = &seam;
  in.db = &db;
  in.scene_id = scene.scene_id;
  // random_seed intentionally left empty (nullopt -> D6 fail-closed).
  EXPECT_THROW(BundleAdjustmentOptimizePipeline(in), ValidationError);

  const auto all = db.FindReconstructionsByScene(scene.scene_id);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].status, "succeeded");
  EXPECT_EQ(all[0].reconstruction_id, ParseUuid(v2.reconstruction_id));
}

// N6: a non-succeeded latest revision cannot feed the correction step — the
// typed error arrives BEFORE any write (no partial supersede of an active
// revision).
TEST_F(P3ProductionE2eTest, N6NonSucceededSourceFailsClosedBeforeAnyWrite) {
  auto& db = project_->db();
  const SceneRow scene = db.FindOrCreateScene(project_id_, "p3_prod_e2e_scene",
                                              "{}", 2000);
  const Reconstruction src = SeedSucceeded(
      db, scene.scene_id,
      MakeReconstruction(FormatUuid(GenerateUuid()), "reconstructing", 100));

  StubOptimizer seam(1.0);
  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &src;
  in.optimizer = &seam;
  in.db = &db;
  in.scene_id = scene.scene_id;
  in.random_seed = "pinned-p3-prod-e2e";
  EXPECT_THROW(BundleAdjustmentOptimizePipeline(in), ValidationError);

  const auto all = db.FindReconstructionsByScene(scene.scene_id);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].status, "reconstructing");
  EXPECT_EQ(all[0].reconstruction_id, ParseUuid(src.reconstruction_id));
}

// §6/P12 (transaction integrity): a WriteTransaction whose body stops before
// Commit is rolled back in full by the destructor — the revision step can
// never leave a half-superseded / dangling-active state.
TEST_F(P3ProductionE2eTest, WriteTransactionRollbackLeavesNoPartialRevision) {
  auto& db = project_->db();
  const SceneRow scene = db.FindOrCreateScene(project_id_, "p3_prod_e2e_scene",
                                              "{}", 2000);

  const Reconstruction v2 = MakeReconstruction(
      FormatUuid(GenerateUuid()), "succeeded", 100);
  ReconstructionRow row;
  row.reconstruction_id = ParseUuid(v2.reconstruction_id);
  row.scene_id = scene.scene_id;
  row.coordinate_frame = v2.coordinate_frame;
  row.status = "succeeded";
  row.created_at_ns = v2.created_at_ns;
  row.document_json = ReconstructionToJson(v2);

  // The insert succeeds inside the transaction, then the body aborts: the
  // destructor must roll BOTH the insert (and any later-staged supersede)
  // back, leaving the table exactly as it was.
  bool threw = false;
  try {
    MetadataDb::WriteTransaction txn(db);
    db.AddReconstruction(row);
    throw std::runtime_error("simulated mid-revision failure");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  EXPECT_TRUE(db.FindReconstructionsByScene(scene.scene_id).empty());
}

}  // namespace