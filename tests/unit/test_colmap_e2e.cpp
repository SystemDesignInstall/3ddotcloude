// COLMAP worker pipeline end-to-end tests (C1-S4, D2/D4). Engines the full
// path: Engine(project, ProcessExecutor(colmap_worker)) + a registered
// single-stage sparse_reconstruction pipeline (images-only) ->
// ExecutionManifest with a CAS sparse_model artifact whose payload is the
// provisional canonical SparseModel document. ADR-020: an identical second
// run is served entirely from the task cache. A missing CAS input fails
// closed in the manifest (no worker dispatch).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_manifest.h"
#include "core/project/project.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/pipeline_definition.h"
#include "engine/workers/process_executor.h"
#include "engine/workers/worker_handle.h"

namespace spatial::engine {
namespace {

#ifndef SPATIAL_COLMAP_WORKER_EXECUTABLE
#error SPATIAL_COLMAP_WORKER_EXECUTABLE must be defined by the test build
#endif
#ifndef SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE
#error SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE must be defined by the test build
#endif

using spatial::core::ArtifactManifest;
using spatial::core::GenerateUuid;
using spatial::core::Project;
using spatial::core::ProjectInfo;
using spatial::core::fs::Iso8601UtcNow;
using nlohmann::json;

class ColmapE2eTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_colmap_e2e_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "colmap-e2e";
    info.created_at = Iso8601UtcNow();
    project_ = std::make_unique<Project>(
        Project::Create(root_ / "demo.spx", info));
  }

  void TearDown() override {
    project_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  static void RegisterSparseReconstruction(PipelineRegistry& registry) {
    PipelineDefinition def;
    def.id = "sparse_reconstruction";
    def.version = "0.1.0";
    def.git_commit = "test";
    def.stages = {
        {"reconstruct", "sparse_reconstruction", "sparse_reconstruction",
         {"image"}, {"reconstruction"}},
    };
    registry.Register(std::move(def));
  }

  ArtifactRef PutImage(const std::string& content) {
    const std::vector<std::uint8_t> bytes(content.begin(), content.end());
    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = "image";
    manifest.producer = {"spatial-platform", "0.1.0", "test"};
    manifest.creation_timestamp = Iso8601UtcNow();
    manifest.file_size = static_cast<std::int64_t>(bytes.size());
    return project_->artifacts().Put(bytes, manifest).content_hash;
  }

  std::unique_ptr<ProcessExecutor> MakeExecutor() {
    return std::make_unique<ProcessExecutor>(
        ResourceProfile{},
        std::vector<std::string>{SPATIAL_COLMAP_WORKER_EXECUTABLE,
                                 SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE},
        "", 5000, &project_->artifacts());
  }

  std::filesystem::path root_;
  std::unique_ptr<Project> project_;
};

TEST_F(ColmapE2eTest, RunPipelineProducesCanonicalSparseModelAndReplays) {
  const ArtifactRef image1 = PutImage("image-one-bytes");
  const ArtifactRef image2 = PutImage("image-two-bytes");

  Engine engine(std::move(*project_), MakeExecutor());
  project_.reset();
  RegisterSparseReconstruction(engine.registry());

  const auto first =
      engine.RunPipeline("sparse_reconstruction", {image1, image2}, "{}");

  EXPECT_EQ(first.status, "succeeded");
  EXPECT_EQ(first.pipeline_id, "sparse_reconstruction");
  ASSERT_EQ(first.stages.size(), 1u);
  EXPECT_EQ(first.stages[0].stage_id, "reconstruct");
  EXPECT_EQ(first.stages[0].status, "succeeded");
  // D1: the generic implementation label lands in the manifest provenance.
  EXPECT_EQ(first.stages[0].implementation, "process");
  EXPECT_FALSE(first.stages[0].cache_hit);
  ASSERT_EQ(first.stages[0].output_refs.size(), 1u);
  const ArtifactRef output = first.stages[0].output_refs.front();
  EXPECT_TRUE(engine.project().artifacts().Has(output));

  // The produced artifact is the canonical Reconstruction v2 document.
  const auto indexed = engine.project().db().FindArtifactByHash(output);
  ASSERT_TRUE(indexed.has_value());
  const auto manifest = engine.project().artifacts().ReadManifest(indexed->artifact_id);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->type, "reconstruction");
  EXPECT_EQ(manifest->schema_version, 2);
  ASSERT_EQ(manifest->input_artifact_hashes.size(), 2u);
  const auto payload = engine.project().artifacts().Get(output);
  ASSERT_TRUE(payload.has_value());
  const json document =
      json::parse(std::string(payload->begin(), payload->end()));
  EXPECT_EQ(document["schema_version"].get<int>(), 2);
  EXPECT_TRUE(document.contains("reconstruction_id"));
  ASSERT_EQ(document["cameras"].size(), 1u);
  EXPECT_EQ(document["cameras"][0]["intrinsic_model"].get<std::string>(),
            "pinhole");
  EXPECT_EQ(document["images"].size(), 2u);
  EXPECT_EQ(document["points3D"].size(), 1u);

  // ADR-020 replay: identical inputs + config -> identical pipeline identity
  // and output, served entirely from the task cache (no worker re-run).
  const auto second =
      engine.RunPipeline("sparse_reconstruction", {image1, image2}, "{}");
  EXPECT_EQ(second.pipeline_hash, first.pipeline_hash);
  EXPECT_EQ(second.stages[0].output_refs, first.stages[0].output_refs);
  EXPECT_TRUE(second.stages[0].cache_hit);
}

TEST_F(ColmapE2eTest, MissingCasInputFailsClosedInManifest) {
  const ArtifactRef bogus = std::string(64, 'd');  // never in the CAS

  Engine engine(std::move(*project_), MakeExecutor());
  project_.reset();
  RegisterSparseReconstruction(engine.registry());

  // The stage cannot materialize a CAS hash that does not exist: the task
  // fails closed on the host before any worker dispatch, and the failure is
  // reflected in the manifest (no exception escapes RunPipeline). The stage's
  // output_refs keep their compile-time placeholder — never a real CAS ref.
  const auto manifest =
      engine.RunPipeline("sparse_reconstruction", {bogus}, "{}");
  EXPECT_EQ(manifest.status, "failed");
  ASSERT_EQ(manifest.stages.size(), 1u);
  EXPECT_EQ(manifest.stages[0].status, "failed");
  ASSERT_EQ(manifest.stages[0].output_refs.size(), 1u);
  EXPECT_FALSE(engine.project().artifacts().Has(
      manifest.stages[0].output_refs.front()));
}

}  // namespace
}  // namespace spatial::engine
