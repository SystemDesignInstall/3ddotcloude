// P3.1 — the p3_sparse_correction pipeline through Engine::RunPipeline ONLY.
//
// The production sparse-correction chain is driven entirely through the public
// engine surface (docs/architecture/P3.1-production-sparse-correction-closure
// .md §2/§4):
//
//   stage 1 sparse_reconstruction (images => colmap worker subprocess;
//     reconstruction => CAS passthrough)
//     -> stage 2 host COMMIT + stage-6 bundle adjustment (seam, D5 gate, P14
//        ordering, pinned seed; DB-committing stage never replayed,
//        CachePolicy::kNever).
//
// Nothing here calls a host orchestration function directly — the by-pass-exec
// the P3.1 milestone closes. Negatives N1/N2/N3/N5 enter through RunPipeline
// and assert the same invariants as the host-level suite: the manifest is
// "failed" and nothing beyond the commit is written (P12). N4 and N6 remain
// covered by the host-level stage-5 / BundleAdjustmentOptimizePipeline tests:
// N4 needs a metric closure + trajectory document and N6 is structurally
// unreachable at this surface (a commit always leaves a succeeded latest).

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
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
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/production_pipelines.h"
#include "engine/pipeline/sparse_correction_runner.h"

#ifndef SPATIAL_COLMAP_WORKER_EXECUTABLE
#error SPATIAL_COLMAP_WORKER_EXECUTABLE must be defined by the test build
#endif
#ifndef SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE
#error SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE must be defined by the test build
#endif

namespace {

using spatial::core::ArtifactManifest;
using spatial::core::GenerateUuid;
using spatial::core::FormatUuid;
using spatial::core::ParseUuid;
using spatial::core::MetadataDb;
using spatial::core::Project;
using spatial::core::ProjectInfo;
using spatial::core::Reconstruction;
using spatial::core::ReconstructionFromJson;
using spatial::core::ReconstructionRow;
using spatial::core::ReconstructionToJson;
using spatial::core::Uuid;
using spatial::core::fs::Iso8601UtcNow;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::BundleAdjustmentTrace;
using spatial::core::geometry::ReconstructionOptimizer;
using spatial::engine::Engine;
using spatial::engine::kSparseCorrectionPipelineId;
using spatial::engine::MakeSparseCorrectionRunner;
using spatial::engine::RegisterProductionPipelines;
using spatial::engine::SparseCorrectionProfile;
using spatial::engine::SparseCorrectionSeams;
using nlohmann::json;

// Canonical (schema-valid) reconstruction document with one camera + one image.
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

  spatial::core::ReconImage image;
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

// Deterministic stage-6 seam stand-in (fake optics on the seam boundary; the
// seam never touches a backend). v_n+1 carries created_at = source + 100 so the
// revision clock stays strictly increasing (P14 ordering observable in rows).
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

class P3SparseCorrectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_p3_sparse_correction_" +
             std::to_string(std::time(nullptr)) + "_" + std::to_string(rand()));
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "p3-sparse-correction";
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

  // Builds the engine with the sparse-correction host runner installed. The
  // runner captures db/store (heap-stable unique_ptrs) BEFORE the Project is
  // moved into the Engine (P3.1 §1.4).
  std::unique_ptr<Engine> MakeEngine(ReconstructionOptimizer* ba_optimizer,
                                     bool with_worker) {
    std::vector<std::string> worker;
    if (with_worker) {
      worker.push_back(SPATIAL_COLMAP_WORKER_EXECUTABLE);
      worker.push_back(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE);
    }
    auto& db = project_->db();
    auto& store = project_->artifacts();
    SparseCorrectionSeams seams;
    seams.ba_optimizer = ba_optimizer;
    auto engine = std::make_unique<Engine>(
        std::move(*project_), MakeSparseCorrectionRunner(db, store, seams,
                                                         std::move(worker)),
        SparseCorrectionProfile());
    project_.reset();
    RegisterProductionPipelines(engine->registry());
    return engine;
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

  json RunConfig(const std::string& scene_name) const {
    return {{"project_id", FormatUuid(project_id_)},
            {"scene_name", scene_name},
            {"random_seed", "pinned-p3-sparse-correction"}};
  }

  std::filesystem::path root_;
  std::unique_ptr<Project> project_;
  Uuid project_id_{};
};

// Golden: the FULL chain (images -> worker subprocess -> host commit -> BA ->
// v4) enters the engine through RunPipeline only, with the P14 lifecycle,
// D6 seed, and CAS persistence asserted.
TEST_F(P3SparseCorrectionTest, GoldenImagesModeFullChainThroughEngineSurface) {
  const std::string image1 = PutImage("p3-sparse-correction-image-1");
  const std::string image2 = PutImage("p3-sparse-correction-image-2");

  StubOptimizer improving(1.0);
  auto engine = MakeEngine(&improving, /*with_worker=*/true);

  const auto manifest = engine->RunPipeline(kSparseCorrectionPipelineId,
                                            {image1, image2},
                                            RunConfig("p3_sparse_golden").dump());
  ASSERT_EQ(manifest.status, "succeeded");
  ASSERT_EQ(manifest.stages.size(), 2u);
  EXPECT_EQ(manifest.stages[0].status, "succeeded");
  EXPECT_EQ(manifest.stages[1].status, "succeeded");
  ASSERT_EQ(manifest.stages[1].output_refs.size(), 1u);
  const std::string out_hash = manifest.stages[1].output_refs.front();
  EXPECT_TRUE(engine->project().artifacts().Has(out_hash));

  // P14 lifecycle + ordering: the committed v2 is superseded by the v4; the
  // v4 is the ONLY succeeded (and therefore latest) revision; timestamps are
  // strictly increasing; identities are distinct.
  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  const auto all =
      engine->project().db().FindReconstructionsByScene(scene->scene_id);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].status, "superseded");
  EXPECT_EQ(all[1].status, "succeeded");
  EXPECT_LT(all[0].created_at_ns, all[1].created_at_ns);
  EXPECT_NE(all[0].reconstruction_id, all[1].reconstruction_id);
  const auto latest =
      engine->project().db().QueryLatestReconstructionByScene(scene->scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->status, "succeeded");
  EXPECT_EQ(latest->reconstruction_id, all[1].reconstruction_id);

  // The terminal artifact is the canonical succeeded v4 document.
  const auto payload = engine->project().artifacts().Get(out_hash);
  ASSERT_TRUE(payload.has_value());
  const json doc =
      json::parse(std::string(payload->begin(), payload->end()));
  EXPECT_EQ(doc["schema_version"].get<int>(), 2);
  EXPECT_EQ(doc["status"].get<std::string>(), "succeeded");

  // D6: the pinned seed reached the seam identically.
  ASSERT_EQ(improving.seen_seeds_.size(), 1u);
  EXPECT_EQ(improving.seen_seeds_[0], "pinned-p3-sparse-correction");
}

// Golden: reconstruction mode needs NO worker binary — the CAS reconstruction
// document passes through stage 1 and the host commits + corrects it. Proves
// the passthrough output is content-identical and the same P14 lifecycle.
TEST_F(P3SparseCorrectionTest, GoldenReconstructionModeWithoutWorker) {
  const Reconstruction source =
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100);
  const std::string payload_hash =
      PutReconstructionPayload(ReconstructionToJson(source));

  StubOptimizer improving(1.0);
  auto engine = MakeEngine(&improving, /*with_worker=*/false);

  const auto manifest = engine->RunPipeline(
      kSparseCorrectionPipelineId, {payload_hash},
      RunConfig("p3_sparse_recon_mode").dump());
  ASSERT_EQ(manifest.status, "succeeded");
  ASSERT_EQ(manifest.stages.size(), 2u);
  // Stage 1 is the identity passthrough: the same canonical bytes come back.
  ASSERT_EQ(manifest.stages[0].output_refs.size(), 1u);
  EXPECT_EQ(manifest.stages[0].output_refs.front(), payload_hash);
  ASSERT_EQ(manifest.stages[1].output_refs.size(), 1u);

  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  const auto all =
      engine->project().db().FindReconstructionsByScene(scene->scene_id);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0].status, "superseded");
  EXPECT_EQ(all[1].status, "succeeded");
  EXPECT_LT(all[0].created_at_ns, all[1].created_at_ns);
  const auto latest =
      engine->project().db().QueryLatestReconstructionByScene(scene->scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, all[1].reconstruction_id);
}

// N1: a non-improving seam fails the D5 gate — the manifest is "failed", no
// v4 is inserted and no supersede happens; the committed source revision
// remains the ONLY succeeded revision (nothing is written past the commit).
TEST_F(P3SparseCorrectionTest, N1NonImprovingGateFailsThroughSurface) {
  const Reconstruction source =
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100);
  const std::string payload_hash =
      PutReconstructionPayload(ReconstructionToJson(source));

  StubOptimizer non_improving(2.0);  // rms_after == rms_before: not < 0.9*2.0
  auto engine = MakeEngine(&non_improving, /*with_worker=*/false);

  const auto manifest = engine->RunPipeline(kSparseCorrectionPipelineId,
                                            {payload_hash},
                                            RunConfig("p3_sparse_n1").dump());
  EXPECT_EQ(manifest.status, "failed");
  EXPECT_EQ(manifest.stages[0].status, "succeeded");

  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  const auto all =
      engine->project().db().FindReconstructionsByScene(scene->scene_id);
  ASSERT_EQ(all.size(), 1u);  // the committed v2 ONLY — no v4, no supersede
  EXPECT_EQ(all[0].status, "succeeded");
  const auto latest =
      engine->project().db().QueryLatestReconstructionByScene(scene->scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, all[0].reconstruction_id);
}

// N2: a CAS input that does not exist fails closed on the host in the
// manifest; nothing is committed to the revision history.
TEST_F(P3SparseCorrectionTest, N2MissingInputFailsClosedAndNothingCommitted) {
  const std::string bogus(64, 'e');  // never in this project's CAS
  auto engine = MakeEngine(nullptr, /*with_worker=*/false);

  const auto manifest = engine->RunPipeline(kSparseCorrectionPipelineId,
                                            {bogus},
                                            RunConfig("p3_sparse_n2").dump());
  EXPECT_EQ(manifest.status, "failed");
  ASSERT_EQ(manifest.stages.size(), 2u);
  EXPECT_EQ(manifest.stages[0].status, "failed");

  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  if (scene.has_value()) {
    EXPECT_TRUE(
        engine->project().db().FindReconstructionsByScene(scene->scene_id)
            .empty());
  }
}

// N3: a canonical but non-succeeded reconstruction document fails closed on the
// correction stage BEFORE any database write — the scene is never even created.
TEST_F(P3SparseCorrectionTest, N3NonSucceededPayloadFailsClosed) {
  const Reconstruction non_succeeded =
      MakeReconstruction(FormatUuid(GenerateUuid()), "reconstructing", 100);
  const std::string payload_hash =
      PutReconstructionPayload(ReconstructionToJson(non_succeeded));

  StubOptimizer improving(1.0);
  auto engine = MakeEngine(&improving, /*with_worker=*/false);

  const auto manifest = engine->RunPipeline(kSparseCorrectionPipelineId,
                                            {payload_hash},
                                            RunConfig("p3_sparse_n3").dump());
  EXPECT_EQ(manifest.status, "failed");
  EXPECT_EQ(manifest.stages[0].status, "succeeded");  // passthrough forward

  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  if (scene.has_value()) {
    EXPECT_TRUE(
        engine->project().db().FindReconstructionsByScene(scene->scene_id)
            .empty());
  }
}

// N5: an absent pinned seed fails closed (D6) BEFORE any database write.
TEST_F(P3SparseCorrectionTest, N5UnpinnedSeedFailsClosedBeforeAnyWrite) {
  const Reconstruction source =
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100);
  const std::string payload_hash =
      PutReconstructionPayload(ReconstructionToJson(source));

  StubOptimizer improving(1.0);
  auto engine = MakeEngine(&improving, /*with_worker=*/false);

  json cfg = {{"project_id", FormatUuid(project_id_)},
              {"scene_name", "p3_sparse_n5"}};  // random_seed intentionally absent
  const auto manifest = engine->RunPipeline(kSparseCorrectionPipelineId,
                                            {payload_hash}, cfg.dump());
  EXPECT_EQ(manifest.status, "failed");

  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  if (scene.has_value()) {
    EXPECT_TRUE(
        engine->project().db().FindReconstructionsByScene(scene->scene_id)
            .empty());
  }
}

// N1 (GO N1) — damaged SHA-256: a REAL, previously-Put CAS payload whose stored
// bytes no longer match the recorded content hash. Every ArtifactStore::Get
// re-verifies SHA-256 before handing bytes out, so the read fails closed: the
// manifest is "failed", the corrupt payload is QUARANTINED and flagged
// "degraded", and NOTHING is committed (no scene, no revision row).
TEST_F(P3SparseCorrectionTest, N1_DamagedSha256FailsClosedAndQuarantines) {
  const Reconstruction source =
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100);
  const std::string payload_hash =
      PutReconstructionPayload(ReconstructionToJson(source));

  // Logical content truncation under the SAME content hash: the manifest row is
  // untouched, only the payload bytes are damaged (test_artifact_store pattern).
  const std::filesystem::path payload_path =
      root_ / "demo.spx" / "artifacts" / "cas" / payload_hash.substr(0, 2) /
      payload_hash;
  ASSERT_TRUE(std::filesystem::exists(payload_path));
  {
    std::ofstream out(payload_path, std::ios::binary | std::ios::trunc);
    out.put(static_cast<char>(0xff));
  }

  StubOptimizer improving(1.0);
  auto engine = MakeEngine(&improving, /*with_worker=*/false);

  const auto manifest = engine->RunPipeline(kSparseCorrectionPipelineId,
                                            {payload_hash},
                                            RunConfig("p3_sparse_n1").dump());
  EXPECT_EQ(manifest.status, "failed");
  ASSERT_EQ(manifest.stages.size(), 2u);
  EXPECT_EQ(manifest.stages[0].status, "failed");  // the read re-verifies SHA

  // The corrupt payload was moved to quarantine; its index entry is flagged.
  EXPECT_FALSE(std::filesystem::exists(payload_path));
  const auto index = engine->project().db().FindArtifactByHash(payload_hash);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->validation_status, "degraded");

  // Fail-closed before any domain mutation: no scene, no revisions.
  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  if (scene.has_value()) {
    EXPECT_TRUE(
        engine->project().db().FindReconstructionsByScene(scene->scene_id)
            .empty());
  }
}

// N4 (GO N4) — "no latest succeeded reconstruction": a scene whose NEWEST
// revision (ANY status) is not succeeded cannot be superseded by a correction.
// QueryLatestReconstructionByScene filters succeeded rows only, so the runner
// inspects the raw history BEFORE any database write (P12). The refusal writes
// NOTHING: the pre-seeded scene + reconstructing revision stay byte-identical
// and no closure/graph/result/revision evidence appears.
TEST_F(P3SparseCorrectionTest, N4_NonSucceededNewestRejectedBeforeAnyWrite) {
  // Pre-seed the scene with a STUCK (reconstructing) newest revision.
  const Reconstruction stuck =
      MakeReconstruction(FormatUuid(GenerateUuid()), "reconstructing", 100);
  const json stuck_doc = ReconstructionToJson(stuck);
  const auto scene =
      project_->db().FindOrCreateScene(project_id_, "p3_sparse_n4", "{}", 1000);
  ReconstructionRow stuck_row;
  stuck_row.reconstruction_id = ParseUuid(stuck.reconstruction_id);
  stuck_row.scene_id = scene.scene_id;
  stuck_row.coordinate_frame = stuck.coordinate_frame;
  stuck_row.status = stuck.status;
  stuck_row.created_at_ns = stuck.created_at_ns;
  stuck_row.document_json = stuck_doc.dump();
  project_->db().AddReconstruction(stuck_row);

  const Reconstruction source =
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100);
  const std::string payload_hash =
      PutReconstructionPayload(ReconstructionToJson(source));

  StubOptimizer improving(1.0);
  auto engine = MakeEngine(&improving, /*with_worker=*/false);

  const auto manifest = engine->RunPipeline(kSparseCorrectionPipelineId,
                                            {payload_hash},
                                            RunConfig("p3_sparse_n4").dump());
  EXPECT_EQ(manifest.status, "failed");
  ASSERT_EQ(manifest.stages.size(), 2u);
  EXPECT_EQ(manifest.stages[0].status, "succeeded");  // passthrough forwards
  EXPECT_EQ(manifest.stages[1].status, "failed");     // all-status preflight

  // Nothing mutated: the pre-seeded rows are the ONLY entries, byte-identical.
  const auto after =
      engine->project().db().FindReconstructionsByScene(scene.scene_id);
  ASSERT_EQ(after.size(), 1u);
  EXPECT_EQ(after[0].status, "reconstructing");
  EXPECT_EQ(after[0].document_json, stuck_doc.dump());
  // A non-succeeded newest is excluded from the succeeded-latest view entirely.
  EXPECT_FALSE(
      engine->project().db().QueryLatestReconstructionByScene(scene.scene_id)
          .has_value());
  // No correction evidence was written either.
  EXPECT_TRUE(
      engine->project().db().FindArtifactsByType("loop_closure").empty());
}

// N5 (GO N5) — deterministic re-run: a second IDENTICAL RunPipeline invocation
// (same payload, same config) has the same deterministic run identity
// (pipeline_hash) and the same pass-through output, and it NEVER duplicates or
// diverges the revision lineage. The committed identity is already registered,
// so the DB-committing stage refuses the duplicate commit and the first run's
// state is left exactly as it was — no double-publish, no divergent latest.
TEST_F(P3SparseCorrectionTest, N5_DeterministicReplayNeverDuplicatesRevisions) {
  const Reconstruction source =
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100);
  const std::string payload_hash =
      PutReconstructionPayload(ReconstructionToJson(source));
  const std::string cfg = RunConfig("p3_sparse_n5").dump();

  StubOptimizer improving(1.0);
  auto engine = MakeEngine(&improving, /*with_worker=*/false);

  const auto m1 =
      engine->RunPipeline(kSparseCorrectionPipelineId, {payload_hash}, cfg);
  ASSERT_EQ(m1.status, "succeeded");
  const auto m2 =
      engine->RunPipeline(kSparseCorrectionPipelineId, {payload_hash}, cfg);
  EXPECT_EQ(m2.status, "failed");  // duplicate committed identity, fail-closed

  // Deterministic run identity + pass-through output across both runs.
  EXPECT_EQ(m1.pipeline_hash, m2.pipeline_hash);
  ASSERT_EQ(m1.stages.size(), 2u);
  ASSERT_EQ(m2.stages.size(), 2u);
  EXPECT_EQ(m1.stages[0].output_refs, m2.stages[0].output_refs);
  ASSERT_EQ(m1.stages[0].output_refs.size(), 1u);
  EXPECT_EQ(m1.stages[0].output_refs.front(), payload_hash);

  // The re-run published NOTHING: the lineage is exactly the first run's
  // (committed superseded + v4 succeeded), unchanged and not duplicated.
  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  const auto rows =
      engine->project().db().FindReconstructionsByScene(scene->scene_id);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].status, "superseded");
  EXPECT_EQ(rows[1].status, "succeeded");
  // The succeeded revision is byte-identical to run1's terminal v4 artifact.
  ASSERT_EQ(m1.stages[1].output_refs.size(), 1u);
  const auto v4_bytes =
      engine->project().artifacts().Get(m1.stages[1].output_refs.front());
  ASSERT_TRUE(v4_bytes.has_value());
  EXPECT_EQ(rows[1].document_json,
            std::string(v4_bytes->begin(), v4_bytes->end()));
  const auto latest =
      engine->project().db().QueryLatestReconstructionByScene(scene->scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, rows[1].reconstruction_id);
}

// N6 (GO N6) — rollback + cache semantics. The DB-COMMITTING correction stage
// is CachePolicy::kNever (a replay would skip the writes): on an identical
// re-run it MUST re-execute (cache_hit=false) while the pure-CAS
// sparse_reconstruction stage replays from the task cache (cache_hit=true).
// The re-run's refusal rolls back to the prior CONSISTENT state: the failed
// attempt wrote nothing — same revision rows, same succeeded latest (P12).
TEST_F(P3SparseCorrectionTest, N6_CommittedStageNeverReplayedAndRerunRollsBack) {
  const Reconstruction source =
      MakeReconstruction(FormatUuid(GenerateUuid()), "succeeded", 100);
  const std::string payload_hash =
      PutReconstructionPayload(ReconstructionToJson(source));
  const std::string cfg = RunConfig("p3_sparse_n6").dump();

  StubOptimizer improving(1.0);
  auto engine = MakeEngine(&improving, /*with_worker=*/false);

  const auto m1 =
      engine->RunPipeline(kSparseCorrectionPipelineId, {payload_hash}, cfg);
  ASSERT_EQ(m1.status, "succeeded");
  ASSERT_EQ(m1.stages.size(), 2u);
  EXPECT_FALSE(m1.stages[0].cache_hit);  // first run executes both stages
  EXPECT_FALSE(m1.stages[1].cache_hit);

  const auto scene = engine->project().db().FindSceneByProject(project_id_);
  ASSERT_TRUE(scene.has_value());
  const auto before =
      engine->project().db().FindReconstructionsByScene(scene->scene_id);
  ASSERT_EQ(before.size(), 2u);
  const std::string before_latest_doc = before[1].document_json;

  const auto m2 =
      engine->RunPipeline(kSparseCorrectionPipelineId, {payload_hash}, cfg);
  EXPECT_EQ(m2.status, "failed");
  ASSERT_EQ(m2.stages.size(), 2u);
  EXPECT_TRUE(m2.stages[0].cache_hit);   // CAS passthrough replayed from cache
  EXPECT_FALSE(m2.stages[1].cache_hit);  // kNever: recomputed, never replayed
  EXPECT_EQ(m2.stages[1].status, "failed");

  // Rollback: the failed re-run left the prior consistent state untouched —
  // same two revisions, same succeeded latest, no partial or duplicate writes.
  const auto after =
      engine->project().db().FindReconstructionsByScene(scene->scene_id);
  ASSERT_EQ(after.size(), 2u);
  EXPECT_EQ(after[0].status, "superseded");
  EXPECT_EQ(after[1].status, "succeeded");
  EXPECT_EQ(after[1].document_json, before_latest_doc);
  const auto latest =
      engine->project().db().QueryLatestReconstructionByScene(scene->scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, after[1].reconstruction_id);
}

}  // namespace