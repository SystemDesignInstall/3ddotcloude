// P3-impl-8c engine orchestration-stage tests (P14/P16; docs/architecture/
// P3-impl-8c-bundle-adjustment-readiness.md §5).
//
// Proves the REAL engine persistence path around the injected
// core::geometry::ReconstructionOptimizer seam:
//   v3 (succeeded, latest) + observations + pinned seed -> seam -> v4
//   -> AddReconstruction(v4, "succeeded") THEN
//      SetReconstructionStatus(v3, "superseded")          (exact P14 order)
// and the D5 acceptance gate: a non-improving seam result (RMS_after not
// strictly < 0.9 * RMS_before) causes NO v4 insert and NO supersede — the v3
// stays the only succeeded revision. Also proves the fail-closed preconditions
// (missing deps -> ran=false; nullopt seed / non-succeeded source / seam that
// returns the v3 id -> typed ValidationError thrown BEFORE any write).
//
// The seam here is a deterministic stub (fake optics on the seam boundary,
// matches 8a/8b stub usage); it never touches a backend or a subprocess.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "core/errors/project_error.h"
#include "core/geometry/reconstruction_optimizer.h"
#include "core/reconstruction/reconstruction.h"
#include "core/reconstruction/reconstruction_json.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/bundle_adjustment_optimize_pipeline.h"

namespace {

using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;
using spatial::core::ParseUuid;
using spatial::core::ReconImage;
using spatial::core::Reconstruction;
using spatial::core::ReconstructionFromJson;
using spatial::core::ReconstructionRow;
using spatial::core::ReconstructionToJson;
using spatial::core::Uuid;
using spatial::core::ValidationError;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::BundleAdjustmentTrace;
using spatial::core::geometry::ReconstructionOptimizer;
using spatial::engine::BundleAdjustmentOptimizePipeline;
using spatial::engine::BundleAdjustmentOptimizePipelineInput;
using spatial::engine::BundleAdjustmentOptimizePipelineResult;

Reconstruction MakeV3(const std::string& id, const std::string& status) {
  Reconstruction rec;
  rec.reconstruction_id = id;
  rec.scene_id = "22222222-2222-4222-8222-222222222222";
  rec.session_ids = {"33333333-3333-4333-8333-333333333333"};
  rec.coordinate_frame = "reconstruction_0";
  rec.status = status;
  rec.created_at_ns = 100;
  rec.provenance.backend.name = "colmap";
  rec.provenance.backend.version = "v3";
  rec.provenance.configuration_hash =
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

  Reconstruction recon_cam;
  recon_cam.cameras.clear();
  // One first-class camera + one image so ReconstructionToJson round-trips a
  // realistic (but compact) document.
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
  image.pose.rotation_xyzw = {-0.0, 0.0, 0.0, 1.0};
  image.pose.translation_xyz = {1.0, 2.0, 3.0};
  image.detected = true;
  rec.images.push_back(image);
  return rec;
}

// Deterministic seam stand-in. Produces a v4 document from the v3 input with a
// controlled rms_after and (optionally) the same reconstruction id.
class StubOptimizer : public ReconstructionOptimizer {
 public:
  StubOptimizer(double rms_after, bool reuse_v3_id)
      : rms_after_(rms_after), reuse_v3_id_(reuse_v3_id) {}

  BundleAdjustmentResult optimize(const BundleAdjustmentInput& input) override {
    seen_seeds_.push_back(input.random_seed ? *input.random_seed : "");
    BundleAdjustmentTrace trace;
    trace.converged = true;
    trace.iterations = 7;
    trace.rms_before_px = 2.0;
    trace.rms_after_px = rms_after_;
    trace.mean_before_px = 1.8;
    trace.mean_after_px = 0.9 * rms_after_;
    trace.inlier_count_before = 18;
    trace.outlier_count_before = 2;
    trace.inlier_count_after = 20;
    trace.outlier_count_after = 0;
    trace.threshold_px_before = 3.0;
    trace.threshold_px_after = 3.0;

    Reconstruction v4 = input.source;
    v4.reconstruction_id =
        reuse_v3_id_ ? input.source.reconstruction_id : FormatUuid(GenerateUuid());
    v4.status = "succeeded";
    v4.created_at_ns = 200;
    v4.provenance.backend.name = "stub_optimizer";
    v4.provenance.backend.version = "test";
    v4.provenance.backend.adapter_version = "0.1.0";
    return {std::move(v4), trace};
  }

  double rms_after_;
  bool reuse_v3_id_;
  std::vector<std::string> seen_seeds_;
};

class BundleAdjustmentOptimizePipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_baopt_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_.emplace(MetadataDb::Create(root_ / "project.db"));
    project_id_ = GenerateUuid();
    db_->InsertProject(project_id_, "baopt_project", 1, "{}", 1000, "ENU",
                       "world", "{}", "{}");
    scene_ = db_->FindOrCreateScene(project_id_, "baopt_scene", "{}", 2000);
  }

  void TearDown() override {
    if (db_) db_->Close();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Seeds the v3 succeeded revision from `rec` into the DB and returns the
  // freshly parsed copy the pipeline should be invoked with.
  Reconstruction SeedSucceeded(const Reconstruction& rec) {
    ReconstructionRow row;
    row.reconstruction_id = ParseUuid(rec.reconstruction_id);
    row.scene_id = scene_.scene_id;
    row.coordinate_frame = rec.coordinate_frame;
    row.status = rec.status;
    row.created_at_ns = rec.created_at_ns;
    row.document_json = ReconstructionToJson(rec);
    db_->AddReconstruction(row);
    return ReconstructionFromJson(row.document_json);
  }

  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  Uuid project_id_{};
  spatial::core::SceneRow scene_{};
};

TEST_F(BundleAdjustmentOptimizePipelineTest, ImprovingSeamPersistsV4AndSupersedesV3) {
  Reconstruction v3 = SeedSucceeded(MakeV3(FormatUuid(GenerateUuid()), "succeeded"));
  StubOptimizer seam(1.0, /*reuse_v3_id=*/false);

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v3;
  in.optimizer = &seam;
  in.db = &*db_;
  in.scene_id = scene_.scene_id;
  in.random_seed = "pinned-8c-test";

  // Gate: 1.0 < 0.9 * 2.0 strictly.
  const BundleAdjustmentOptimizePipelineResult out =
      BundleAdjustmentOptimizePipeline(in);

  EXPECT_TRUE(out.ran);
  EXPECT_TRUE(out.gate_passed);
  EXPECT_TRUE(out.inserted_v4);
  EXPECT_TRUE(out.superseded_v3);
  ASSERT_TRUE(out.trace.has_value());
  EXPECT_EQ(out.trace->rms_after_px, 1.0);
  ASSERT_TRUE(out.v4.has_value());

  // Exactly one succeeded revision remains, and it is the v4.
  const auto latest = db_->QueryLatestReconstructionByScene(scene_.scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id,
            ParseUuid(out.v4->reconstruction_id));
  EXPECT_EQ(latest->status, "succeeded");
  EXPECT_NE(latest->reconstruction_id, ParseUuid(v3.reconstruction_id));

  // v3 was transitioned (not deleted): FindReconstructionsByScene sees both.
  const std::vector<ReconstructionRow> all =
      db_->FindReconstructionsByScene(scene_.scene_id);
  ASSERT_EQ(all.size(), 2u);
  const auto* v3_row = &all[0];
  const auto* v4_row = &all[1];
  EXPECT_EQ(v3_row->status, "superseded");
  EXPECT_EQ(v4_row->status, "succeeded");

  // The v4 row document round-trips and is a full canonical reconstruction.
  const Reconstruction from_doc =
      ReconstructionFromJson(v4_row->document_json);
  EXPECT_EQ(from_doc.reconstruction_id, FormatUuid(v4_row->reconstruction_id));
  EXPECT_EQ(from_doc.status, "succeeded");
}

TEST_F(BundleAdjustmentOptimizePipelineTest, NonImprovingSeamFailsGateWithoutWrites) {
  Reconstruction v3 = SeedSucceeded(MakeV3(FormatUuid(GenerateUuid()), "succeeded"));
  // Mirror-image failure: rms_after == rms_before (2.0 is NOT < 0.9 * 2.0).
  StubOptimizer seam(2.0, /*reuse_v3_id=*/false);

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v3;
  in.optimizer = &seam;
  in.db = &*db_;
  in.scene_id = scene_.scene_id;
  in.random_seed = "pinned-8c-test";

  const BundleAdjustmentOptimizePipelineResult out =
      BundleAdjustmentOptimizePipeline(in);

  EXPECT_TRUE(out.ran);
  EXPECT_FALSE(out.gate_passed);
  EXPECT_FALSE(out.inserted_v4);
  EXPECT_FALSE(out.superseded_v3);
  EXPECT_FALSE(out.failure.empty());
  ASSERT_TRUE(out.v4.has_value());

  // No v4 row, and v3 remains the succeeded revision.
  const std::vector<ReconstructionRow> all =
      db_->FindReconstructionsByScene(scene_.scene_id);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].reconstruction_id, ParseUuid(v3.reconstruction_id));
  EXPECT_EQ(all[0].status, "succeeded");

  // QueryLatest still returns v3.
  const auto latest = db_->QueryLatestReconstructionByScene(scene_.scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, ParseUuid(v3.reconstruction_id));
}

TEST_F(BundleAdjustmentOptimizePipelineTest, NonFiniteRmsNeverPassesGate) {
  Reconstruction v3 = SeedSucceeded(MakeV3(FormatUuid(GenerateUuid()), "succeeded"));
  StubOptimizer seam(std::numeric_limits<double>::quiet_NaN(),
                     /*reuse_v3_id=*/false);

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v3;
  in.optimizer = &seam;
  in.db = &*db_;
  in.scene_id = scene_.scene_id;
  in.random_seed = "pinned-8c-test";
  EXPECT_FALSE(BundleAdjustmentOptimizePipeline(in).gate_passed);
}

TEST_F(BundleAdjustmentOptimizePipelineTest, MissingDependenciesFailClosed) {
  Reconstruction v3 = SeedSucceeded(MakeV3(FormatUuid(GenerateUuid()), "succeeded"));
  StubOptimizer seam(1.0, false);

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v3;
  in.optimizer = nullptr;
  in.db = &*db_;
  in.scene_id = scene_.scene_id;
  in.random_seed = "pinned-8c-test";
  BundleAdjustmentOptimizePipelineResult out =
      BundleAdjustmentOptimizePipeline(in);
  EXPECT_FALSE(out.ran);
  EXPECT_FALSE(out.failure.empty());

  in.optimizer = &seam;
  in.db = nullptr;
  out = BundleAdjustmentOptimizePipeline(in);
  EXPECT_FALSE(out.ran);
  EXPECT_FALSE(out.failure.empty());

  in.db = &*db_;
  in.source_v3 = nullptr;
  out = BundleAdjustmentOptimizePipeline(in);
  EXPECT_FALSE(out.ran);
  EXPECT_FALSE(out.failure.empty());
}

TEST_F(BundleAdjustmentOptimizePipelineTest, NulloptSeedFailsClosedWithTypedError) {
  Reconstruction v3 = SeedSucceeded(MakeV3(FormatUuid(GenerateUuid()), "succeeded"));
  StubOptimizer seam(1.0, false);

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v3;
  in.optimizer = &seam;
  in.db = &*db_;
  in.scene_id = scene_.scene_id;
  EXPECT_THROW(BundleAdjustmentOptimizePipeline(in), ValidationError);
}

TEST_F(BundleAdjustmentOptimizePipelineTest, NonSucceededSourceRejectedBeforeWrite) {
  // A "reconstructing" v3 cannot feed the stage: strict precondition.
  Reconstruction v3 = SeedSucceeded(MakeV3(FormatUuid(GenerateUuid()), "reconstructing"));
  StubOptimizer seam(1.0, false);

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v3;
  in.optimizer = &seam;
  in.db = &*db_;
  in.scene_id = scene_.scene_id;
  in.random_seed = "pinned-8c-test";
  EXPECT_THROW(BundleAdjustmentOptimizePipeline(in), ValidationError);

  // No writes happened: superseding a non-succeeded row would have thrown
  // StorageError AFTER the v4 insert if the guard were missing.
  const auto all = db_->FindReconstructionsByScene(scene_.scene_id);
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(all[0].status, "reconstructing");
}

TEST_F(BundleAdjustmentOptimizePipelineTest, SeamReturningV3IdRejected) {
  Reconstruction v3 = SeedSucceeded(MakeV3(FormatUuid(GenerateUuid()), "succeeded"));
  StubOptimizer seam(1.0, /*reuse_v3_id=*/true);

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v3;
  in.optimizer = &seam;
  in.db = &*db_;
  in.scene_id = scene_.scene_id;
  in.random_seed = "pinned-8c-test";
  EXPECT_THROW(BundleAdjustmentOptimizePipeline(in), ValidationError);
}

TEST_F(BundleAdjustmentOptimizePipelineTest, SeedReachesTheSeam) {
  Reconstruction v3 = SeedSucceeded(MakeV3(FormatUuid(GenerateUuid()), "succeeded"));
  StubOptimizer seam(1.0, false);

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &v3;
  in.optimizer = &seam;
  in.db = &*db_;
  in.scene_id = scene_.scene_id;
  in.random_seed = "pinned-8c-test";
  BundleAdjustmentOptimizePipeline(in);
  ASSERT_EQ(seam.seen_seeds_.size(), 1u);
  EXPECT_EQ(seam.seen_seeds_[0], "pinned-8c-test");
}

}  // namespace