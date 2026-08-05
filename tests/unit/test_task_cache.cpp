#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"
#include "engine/cache/task_cache.h"
#include "engine/scheduler/scheduler_state_store.h"

#include "tests/unit/engine_test_helpers.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactStore;
using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;

class TaskCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_cache_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = std::make_unique<MetadataDb>(MetadataDb::Create(root_ / "project.db"));
    store_ = std::make_unique<ArtifactStore>(root_ / "artifacts", *db_);
    state_ = std::make_unique<SchedulerStateStore>(*db_);
    cache_ = std::make_unique<TaskCache>(*store_, *state_, "producer-v1",
                                         "commit-abc");
  }

  void TearDown() override {
    cache_.reset();
    state_.reset();
    store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Stores bytes in CAS and returns {content_hash, artifact_uuid}.
  std::pair<std::string, Uuid> StorePayload(const std::string& type,
                                            const std::string& bytes) {
    const std::vector<std::uint8_t> payload(bytes.begin(), bytes.end());
    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = type;
    manifest.producer = {"engine", "0.1.0", "commit-abc"};
    manifest.creation_timestamp = "2026-01-01T00:00:00Z";
    manifest.file_size = static_cast<std::int64_t>(payload.size());
    manifest.mime_type = "application/octet-stream";
    const auto result = store_->Put(payload, manifest);
    return {result.content_hash, result.artifact_uuid};
  }

  TaskInstance DeterministicTask() {
    auto task = test::MakeTask("feature_extract", {}, {});
    task.metadata.deterministic = true;
    task.config_json = R"({"threshold": 0.5})";
    return task;
  }

  std::filesystem::path root_;
  std::unique_ptr<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> store_;
  std::unique_ptr<SchedulerStateStore> state_;
  std::unique_ptr<TaskCache> cache_;
};

TEST_F(TaskCacheTest, CompositeKey) {
  const std::string k =
      TaskCache::ComputeKey("feature_extract", {"a", "b"}, "cfg", "p", "c");
  EXPECT_EQ(k, TaskCache::ComputeKey("feature_extract", {"b", "a"}, "cfg", "p",
                                     "c"));
  EXPECT_NE(k, TaskCache::ComputeKey("reconstruct", {"a", "b"}, "cfg", "p",
                                     "c"));
  EXPECT_NE(k, TaskCache::ComputeKey("feature_extract", {"a", "x"}, "cfg", "p",
                                     "c"));
  EXPECT_NE(k, TaskCache::ComputeKey("feature_extract", {"a", "b"}, "other",
                                     "p", "c"));
  EXPECT_NE(k, TaskCache::ComputeKey("feature_extract", {"a", "b"}, "cfg",
                                     "p2", "c"));
  EXPECT_NE(k, TaskCache::ComputeKey("feature_extract", {"a", "b"}, "cfg", "p",
                                     "c2"));
}

TEST_F(TaskCacheTest, MissThenStoreThenHit) {
  auto task = DeterministicTask();
  const auto [hash, uuid] = StorePayload("feature_points", "point-bytes");
  task.outputs = {hash};

  EXPECT_FALSE(cache_->Lookup(task).has_value());

  cache_->StoreOutput(task, hash);
  const auto entry = cache_->Lookup(task);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->artifact_id, uuid);
  EXPECT_EQ(entry->task_type, "feature_extract");
  EXPECT_EQ(entry->git_commit, "commit-abc");
  EXPECT_EQ(entry->producer_version, "producer-v1");
  const auto output = cache_->ResolveOutput(*entry);
  ASSERT_TRUE(output.has_value());
  EXPECT_EQ(*output, hash);
}

TEST_F(TaskCacheTest, KeyComponentChangeInvalidates) {
  auto task = DeterministicTask();
  const auto [hash, uuid] = StorePayload("feature_points", "point-bytes");
  task.outputs = {hash};
  cache_->StoreOutput(task, hash);
  ASSERT_TRUE(cache_->Lookup(task).has_value());

  task.config_json = R"({"threshold": 0.9})";
  EXPECT_FALSE(cache_->Lookup(task).has_value());
}

TEST_F(TaskCacheTest, NonDeterministicNeverServed) {
  auto task = test::MakeTask("feature_extract", {}, {});
  task.metadata.deterministic = false;  // non-deterministic: never cached
  const auto [hash, uuid] = StorePayload("feature_points", "point-bytes");
  task.outputs = {hash};
  cache_->StoreOutput(task, hash);
  EXPECT_FALSE(cache_->Lookup(task).has_value());
}

TEST_F(TaskCacheTest, CachePolicyNeverBypasses) {
  auto task = DeterministicTask();
  task.metadata.cache = CachePolicy::kNever;
  const auto [hash, uuid] = StorePayload("feature_points", "point-bytes");
  task.outputs = {hash};
  cache_->StoreOutput(task, hash);
  EXPECT_FALSE(cache_->Lookup(task).has_value());
}

TEST_F(TaskCacheTest, InvalidateRemovesEntry) {
  auto task = DeterministicTask();
  const auto [hash, uuid] = StorePayload("feature_points", "point-bytes");
  task.outputs = {hash};
  cache_->StoreOutput(task, hash);
  ASSERT_TRUE(cache_->Lookup(task).has_value());
  cache_->Invalidate(TaskCache::ComputeKey(task.definition.type, task.inputs,
                                           TaskCache::ConfigHash(task),
                                           "producer-v1", "commit-abc"));
  EXPECT_FALSE(cache_->Lookup(task).has_value());
}

}  // namespace
}  // namespace spatial::engine
