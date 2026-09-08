// P3-Production-E2E §3.6 — focused multi-input worker dispatch tests (AC-5).
//
// The engine transport already threads a vector of canonical input refs end to
// end (PipelineCompiler -> Scheduler::DispatchAndAwait -> WorkerExecutor
// TaskRequest.input_refs -> runner). These tests pin that contract through the
// real Engine::RunPipeline producer path with a scripted in-process runner:
//   - one input still works,
//   - multiple inputs are preserved, in declared order,
//   - ordering/identity are deterministic (stable pipeline hash + replay),
//   - a missing required input fails closed (kFailed manifest, no artifact),
//   - an unrelated single-input worker stage remains compatible alongside.
//
// This is the minimum multi-input capability for the loop-closure producer
// stages (7a/7b): one processing request -> multiple canonical input refs ->
// executor receives all declared inputs -> stage executes -> canonical output
// artifact. No scheduler redesign, no generalized workflow engine.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/errors/project_error.h"
#include "core/project/project.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/pipeline_definition.h"
#include "engine/workers/in_process_executor.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactStore;
using spatial::core::GenerateUuid;
using spatial::core::Project;
using spatial::core::ProjectInfo;
using spatial::core::fs::Iso8601UtcNow;

// Scripted runner double. Records every dispatched TaskRequest's input_refs
// (order preserved) and, for the multi/single task types, resolves them against
// the CAS store — failing closed (throwing) when a declared input hash is
// absent. The produced payload is a deterministic function of the ORDINAL
// inputs, so identical ordered input vectors yield an identical CAS hash.
class ScriptedRunner {
 public:
  explicit ScriptedRunner(ArtifactStore* store) : store_(store) {}

  InProcessTaskRunner Make() {
    return [this](const TaskRequest& request,
                  const std::function<void(WorkerEvent)>& emit,
                  const std::function<bool()>&) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        last_input_refs_ = request.input_refs;
        last_task_type_ = request.task_type;
      }

      std::string payload;
      if (request.task_type == "multi_input" ||
          request.task_type == "single_input") {
        for (const auto& ref : request.input_refs) {
          const auto bytes = store_->Get(ref);
          if (!bytes) {
            throw spatial::core::ProjectError(
                spatial::core::ErrorCode::kValidationDomain,
                "missing declared input in CAS: " + ref);
          }
          payload += std::string(bytes->begin(), bytes->end()) + "|";
        }
      } else {
        for (const auto& ref : request.input_refs) {
          payload += ref + "|";
        }
      }
      payload += request.task_type;

      const std::vector<std::uint8_t> payload_bytes(payload.begin(),
                                                    payload.end());
      ArtifactManifest manifest;
      manifest.artifact_uuid = GenerateUuid();
      manifest.type = "feature";
      manifest.producer = {"spatial-platform", "0.1.0", "test"};
      manifest.input_artifact_hashes = request.input_refs;
      manifest.creation_timestamp = Iso8601UtcNow();
      manifest.file_size = static_cast<std::int64_t>(payload_bytes.size());
      manifest.mime_type = "application/octet-stream";
      const auto result = store_->Put(payload_bytes, manifest);

      WorkerEvent produced;
      produced.type = WorkerEventType::kArtifactProduced;
      produced.task_id = request.task_id;
      produced.artifact_ref = result.content_hash;
      emit(produced);

      WorkerEvent completed;
      completed.type = WorkerEventType::kCompleted;
      completed.task_id = request.task_id;
      emit(completed);
    };
  }

  std::vector<ArtifactRef> LastInputRefs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_input_refs_;
  }

  std::string LastTaskType() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_task_type_;
  }

 private:
  ArtifactStore* store_;
  mutable std::mutex mutex_;
  std::vector<ArtifactRef> last_input_refs_;
  std::string last_task_type_;
};

class MultiInputDispatchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_multi_input_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "multi-input";
    info.created_at = Iso8601UtcNow();
    project_ =
        std::make_unique<Project>(Project::Create(root_ / "demo.spx", info));
  }

  void TearDown() override {
    project_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  ArtifactRef Put(const std::string& content) {
    const std::vector<std::uint8_t> bytes(content.begin(), content.end());
    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = "image";
    manifest.producer = {"spatial-platform", "0.1.0", "test"};
    manifest.creation_timestamp = Iso8601UtcNow();
    manifest.file_size = static_cast<std::int64_t>(bytes.size());
    return project_->artifacts().Put(bytes, manifest).content_hash;
  }

  static void RegisterPipelines(PipelineRegistry& registry) {
    PipelineDefinition multi;
    multi.id = "multi_input_pipeline";
    multi.version = "0.1.0";
    multi.git_commit = "test";
    multi.stages = {
        {"multi", "multi_input", "multi_input", {"image", "image"},
         {"feature"}},
    };
    registry.Register(std::move(multi));

    PipelineDefinition single;
    single.id = "single_input_pipeline";
    single.version = "0.1.0";
    single.git_commit = "test";
    single.stages = {
        {"single", "single_input", "single_input", {"image"}, {"feature"}},
    };
    registry.Register(std::move(single));
  }

  std::filesystem::path root_;
  std::unique_ptr<Project> project_;
};

// One input still works through the real Engine::RunPipeline producer path.
TEST_F(MultiInputDispatchTest, OneInputStillWorks) {
  const ArtifactRef image = Put("single-image-bytes");

  ScriptedRunner scripted(&project_->artifacts());
  ResourceProfile profile;
  profile.capabilities = {"multi_input", "single_input"};
  profile.capacity.cores = 4;
  profile.capacity.ram_bytes = std::int64_t{1} << 30;
  profile.max_concurrency = 1;

  Engine engine(std::move(*project_), scripted.Make(), profile);
  project_.reset();
  RegisterPipelines(engine.registry());

  const auto manifest =
      engine.RunPipeline("single_input_pipeline", {image}, "{}");

  EXPECT_EQ(manifest.status, "succeeded");
  ASSERT_EQ(manifest.stages.size(), 1u);
  ASSERT_EQ(scripted.LastInputRefs().size(), 1u);
  EXPECT_EQ(scripted.LastInputRefs()[0], image);
  ASSERT_EQ(manifest.stages[0].output_refs.size(), 1u);
  EXPECT_TRUE(engine.project().artifacts().Has(
      manifest.stages[0].output_refs.front()));
}

// Multiple inputs are preserved, in declared order, through the producer path.
TEST_F(MultiInputDispatchTest, MultipleInputsPreservedInOrder) {
  const ArtifactRef a = Put("image-a");
  const ArtifactRef b = Put("image-b");
  const ArtifactRef c = Put("image-c");
  const std::vector<ArtifactRef> inputs = {a, b, c};

  ScriptedRunner scripted(&project_->artifacts());
  ResourceProfile profile;
  profile.capabilities = {"multi_input", "single_input"};
  profile.capacity.cores = 4;
  profile.capacity.ram_bytes = std::int64_t{1} << 30;
  profile.max_concurrency = 1;

  Engine engine(std::move(*project_), scripted.Make(), profile);
  project_.reset();
  RegisterPipelines(engine.registry());

  const auto manifest =
      engine.RunPipeline("multi_input_pipeline", inputs, "{}");

  EXPECT_EQ(manifest.status, "succeeded");
  ASSERT_EQ(manifest.stages.size(), 1u);
  // The executor received every declared input ref, in the declared order.
  EXPECT_EQ(scripted.LastInputRefs(), inputs);
  ASSERT_EQ(manifest.stages[0].output_refs.size(), 1u);
  EXPECT_TRUE(engine.project().artifacts().Has(
      manifest.stages[0].output_refs.front()));
}

// Ordering/identity are deterministic: identical ordered inputs + config yield
// an identical pipeline hash and an identical produced artifact (replayed from
// the task cache on the second run).
TEST_F(MultiInputDispatchTest, DeterministicIdentityAndReplay) {
  const ArtifactRef a = Put("image-a");
  const ArtifactRef b = Put("image-b");
  const std::vector<ArtifactRef> inputs = {a, b};

  ScriptedRunner scripted(&project_->artifacts());
  ResourceProfile profile;
  profile.capabilities = {"multi_input", "single_input"};
  profile.capacity.cores = 4;
  profile.capacity.ram_bytes = std::int64_t{1} << 30;
  profile.max_concurrency = 1;

  Engine engine(std::move(*project_), scripted.Make(), profile);
  project_.reset();
  RegisterPipelines(engine.registry());

  const auto first =
      engine.RunPipeline("multi_input_pipeline", inputs, "{}");
  EXPECT_EQ(first.status, "succeeded");
  EXPECT_FALSE(first.stages[0].cache_hit);

  const auto second =
      engine.RunPipeline("multi_input_pipeline", inputs, "{}");
  EXPECT_EQ(second.status, "succeeded");
  EXPECT_EQ(second.pipeline_hash, first.pipeline_hash);
  EXPECT_EQ(second.stages[0].output_refs, first.stages[0].output_refs);
  EXPECT_TRUE(second.stages[0].cache_hit);
}

// A missing required input fails closed: the stage never succeeds, no artifact
// is produced, and the manifest reports failed (never a silent no-op).
TEST_F(MultiInputDispatchTest, MissingRequiredInputFailsClosed) {
  const ArtifactRef real = Put("real-image");

  ScriptedRunner scripted(&project_->artifacts());
  ResourceProfile profile;
  profile.capabilities = {"multi_input", "single_input"};
  profile.capacity.cores = 4;
  profile.capacity.ram_bytes = std::int64_t{1} << 30;
  profile.max_concurrency = 1;

  Engine engine(std::move(*project_), scripted.Make(), profile);
  project_.reset();
  RegisterPipelines(engine.registry());

  const ArtifactRef dangling = std::string(64, 'd');  // never in the CAS

  const auto manifest =
      engine.RunPipeline("multi_input_pipeline", {real, dangling}, "{}");

  EXPECT_EQ(manifest.status, "failed");
  ASSERT_EQ(manifest.stages.size(), 1u);
  EXPECT_EQ(manifest.stages[0].status, "failed");
  // The runner preserves the declared multi-input vector even for the failure.
  EXPECT_EQ(scripted.LastInputRefs().size(), 2u);
  // No artifact was produced into the CAS (output placeholder never realized).
  EXPECT_FALSE(engine.project().artifacts().Has(
      manifest.stages[0].output_refs.front()));
}

// An unrelated single-input worker stage remains compatible alongside the
// multi-input stage: both run through the same executor without interference.
TEST_F(MultiInputDispatchTest, UnrelatedSingleInputStageStaysCompatible) {
  const ArtifactRef a = Put("image-a");
  const ArtifactRef b = Put("image-b");
  const ArtifactRef s = Put("single-image");

  ScriptedRunner scripted(&project_->artifacts());
  ResourceProfile profile;
  profile.capabilities = {"multi_input", "single_input"};
  profile.capacity.cores = 4;
  profile.capacity.ram_bytes = std::int64_t{1} << 30;
  profile.max_concurrency = 1;

  Engine engine(std::move(*project_), scripted.Make(), profile);
  project_.reset();
  RegisterPipelines(engine.registry());

  const auto multi =
      engine.RunPipeline("multi_input_pipeline", {a, b}, "{}");
  EXPECT_EQ(multi.status, "succeeded");
  EXPECT_EQ(scripted.LastTaskType(), "multi_input");

  const auto single =
      engine.RunPipeline("single_input_pipeline", {s}, "{}");
  EXPECT_EQ(single.status, "succeeded");
  EXPECT_EQ(scripted.LastTaskType(), "single_input");
  ASSERT_EQ(scripted.LastInputRefs().size(), 1u);
  EXPECT_EQ(scripted.LastInputRefs()[0], s);
}

}  // namespace
}  // namespace spatial::engine
