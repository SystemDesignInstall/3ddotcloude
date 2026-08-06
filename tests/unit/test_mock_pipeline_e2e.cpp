// Mock photogrammetry end-to-end tests (RFC-0003 P1.4, AC-7/AC-8).
// Engine::RunPipeline drives the full path: PipelineCompiler builds the ONLY
// TaskGraph, the in-process mock worker executes it, the ExecutionManifest is
// persisted (migration 0004), and a second run of identical inputs is served
// entirely from the ADR-020 task cache (cache-hit stages record no task_runs).

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "core/artifacts/artifact_manifest.h"
#include "core/project/project.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/mock_photogrammetry.h"
#include "engine/pipeline/pipeline_compiler.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::GenerateUuid;
using spatial::core::Project;
using spatial::core::ProjectInfo;
using spatial::core::fs::Iso8601UtcNow;

class MockPipelineE2eTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_mock_e2e_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "mock-e2e";
    info.created_at = Iso8601UtcNow();
    project_ = std::make_unique<Project>(Project::Create(root_ / "demo.spx", info));
  }

  void TearDown() override {
    project_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  ArtifactRef PutInput(const std::string& content) {
    const std::vector<std::uint8_t> bytes(content.begin(), content.end());
    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = "image";
    manifest.producer = {"spatial-platform", "0.1.0", "test"};
    manifest.creation_timestamp = Iso8601UtcNow();
    manifest.file_size = static_cast<std::int64_t>(bytes.size());
    return project_->artifacts().Put(bytes, manifest).content_hash;
  }

  std::filesystem::path root_;
  std::unique_ptr<Project> project_;
};

TEST_F(MockPipelineE2eTest, RunPipelineSucceedsAndPersistsManifest) {
  const ArtifactRef input = PutInput("image-a");
  Engine engine(std::move(*project_));
  RegisterMockPhotogrammetry(engine.registry());

  const auto manifest =
      engine.RunPipeline("photogrammetry", {input}, R"({"quality":"high"})");

  EXPECT_EQ(manifest.status, "succeeded");
  EXPECT_EQ(manifest.pipeline_id, "photogrammetry");
  EXPECT_EQ(manifest.external_inputs, std::vector<ArtifactRef>{input});
  EXPECT_FALSE(manifest.pipeline_hash.empty());
  EXPECT_NE(manifest.manifest_id, Uuid{});

  ASSERT_EQ(manifest.stages.size(), 3u);
  EXPECT_EQ(manifest.stages[0].stage_id, "feature_extract");
  EXPECT_EQ(manifest.stages[1].stage_id, "reconstruct");
  EXPECT_EQ(manifest.stages[2].stage_id, "validate");
  for (const auto& stage : manifest.stages) {
    EXPECT_EQ(stage.status, "succeeded");
    EXPECT_EQ(stage.implementation, "inprocess");
    EXPECT_FALSE(stage.output_refs.empty());
    EXPECT_EQ(stage.task_hash.size(), 64u);
  }
  // Outputs were threaded through the chain (scheduler §5.9): every produced
  // ref is backed by a real CAS payload.
  for (const auto& stage : manifest.stages) {
    EXPECT_TRUE(engine.project().artifacts().Has(stage.output_refs.front()));
  }

  // Reload from persistence: golden source for Resume/Audit (migration 0004).
  const auto reloaded = engine.LoadManifest(manifest.manifest_id);
  ASSERT_TRUE(reloaded.has_value());
  EXPECT_EQ(reloaded->pipeline_hash, manifest.pipeline_hash);
  EXPECT_EQ(reloaded->stages.size(), manifest.stages.size());
  EXPECT_EQ(reloaded->status, "succeeded");
}

TEST_F(MockPipelineE2eTest, SecondRunIsACacheHitAcrossAllStages) {
  const ArtifactRef input = PutInput("image-a");
  Engine engine(std::move(*project_));
  RegisterMockPhotogrammetry(engine.registry());

  const auto first = engine.RunPipeline("photogrammetry", {input}, "{}");
  const auto second = engine.RunPipeline("photogrammetry", {input}, "{}");

  // AC-8: identical inputs + config -> identical pipeline identity and output.
  EXPECT_EQ(first.pipeline_hash, second.pipeline_hash);
  for (std::size_t i = 0; i < first.stages.size(); ++i) {
    EXPECT_EQ(first.stages[i].output_refs, second.stages[i].output_refs);
  }

  // Every stage was served from cache on the second run...
  for (const auto& stage : second.stages) {
    EXPECT_TRUE(stage.cache_hit) << "stage " << stage.stage_id
                                 << " should have been a cache hit";
  }
  for (const auto& stage : first.stages) {
    EXPECT_FALSE(stage.cache_hit) << "stage " << stage.stage_id
                                  << " must not hit on a cold cache";
  }

  // ...and a cache hit is a replay, not an execution: no task_runs rows for
  // the replayed stage tasks (RFC-0003 §5.10, ADR-020).
  const char* kCountRuns =
      "SELECT COUNT(*) FROM task_runs WHERE task_id = ?1";
  for (const auto& stage : second.stages) {
    sqlite3* db = engine.project().db().db();
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, kCountRuns, -1, &stmt, nullptr), SQLITE_OK);
    sqlite3_bind_blob(stmt, 1, stage.task_id.data(),
                      static_cast<int>(stage.task_id.size()), SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(stmt, 0), 0)
        << "cache-hit stage " << stage.stage_id << " must not have a run row";
    sqlite3_finalize(stmt);
  }
}

TEST_F(MockPipelineE2eTest, RunGraphRunsAdHocDagAndPersistsJobTasks) {
  const ArtifactRef input = PutInput("image-a");

  PipelineDefinition def;
  def.id = "two-stage";
  def.version = "0.1.0";
  def.git_commit = "test";
  def.stages = {
      {"feature_extract", "feature_extraction", "feature_extract",
       {"image"}, {"keypoints"}},
      {"reconstruct", "reconstruction", "reconstruct",
       {"keypoints"}, {"point_cloud"}},
  };
  PipelineCompiler compiler("0.1.0", "test");
  auto plan = compiler.Compile(def, {input}, "{}", DemoWorkerProfile(),
                               "inprocess");

  Engine engine(std::move(*project_));
  RegisterMockPhotogrammetry(engine.registry());

  // AC-7: the DAG (a compiled plan graph) is handed to the engine verbatim.
  const Uuid job_id = engine.RunGraph(*plan.graph, plan.external_inputs);
  EXPECT_EQ(job_id, plan.job_id);

  const auto tasks = engine.LoadJobTasks(job_id);
  ASSERT_EQ(tasks.size(), 2u);
  for (const auto& task : tasks) {
    EXPECT_EQ(task.state, TaskStatus::kSucceeded);
  }

  // No pipeline manifest is produced for ad hoc DAGs.
  EXPECT_TRUE(engine.LoadManifest(job_id) == std::nullopt);
}

}  // namespace
}  // namespace spatial::engine
