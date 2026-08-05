#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/errors/project_error.h"
#include "core/storage/metadata_db.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/execution/execution_record.h"
#include "engine/scheduler/scheduler_state_store.h"
#include "engine/task/task_graph.h"

#include "tests/unit/engine_test_helpers.h"

namespace spatial::engine {
namespace {

using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;

class SchedulerStateStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_sched_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = std::make_unique<MetadataDb>(
        MetadataDb::Create(root_ / "project.db"));
    store_ = std::make_unique<SchedulerStateStore>(*db_);
  }

  void TearDown() override {
    store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
  std::unique_ptr<MetadataDb> db_;
  std::unique_ptr<SchedulerStateStore> store_;
};

TEST_F(SchedulerStateStoreTest, SaveAndLoadGraph) {
  TaskGraph graph(GenerateUuid());
  auto a = graph.AddTask(test::MakeTask("a", {}, {"h1"}));
  auto b = graph.AddTask(test::MakeTask("b", {"h1"}, {"h2"}));
  graph.AddDependency(b, a);

  store_->SaveGraph(graph);

  const auto tasks = store_->LoadTasks(graph.job_id());
  ASSERT_EQ(tasks.size(), 2u);
  const auto deps = store_->LoadDependencies(graph.job_id());
  ASSERT_EQ(deps.size(), 1u);
  EXPECT_EQ(deps[0].first, b);
  EXPECT_EQ(deps[0].second, a);
  // The task row round-trips through spec_json.
  EXPECT_EQ(store_->FindTask(a)->definition.type, "a");
}

TEST_F(SchedulerStateStoreTest, UpsertTaskPersistsTransitions) {
  TaskGraph graph(GenerateUuid());
  auto a = graph.AddTask(test::MakeTask("a"));
  store_->SaveGraph(graph);

  auto task = store_->FindTask(a);
  ASSERT_TRUE(task.has_value());
  task->state = TaskStatus::kRunning;
  store_->UpsertTask(graph.job_id(), *task);
  EXPECT_EQ(store_->FindTask(a)->state, TaskStatus::kRunning);
}

TEST_F(SchedulerStateStoreTest, RecordAndLoadRuns) {
  const auto task_id = GenerateUuid();
  auto task = test::MakeTask("feature_extract");
  task.id = task_id;
  TaskGraph graph(GenerateUuid());
  graph.AddTask(task);
  store_->SaveGraph(graph);  // task_runs.task_id references tasks(task_id)

  ExecutionRecord record;
  record.id = GenerateUuid();
  record.task_id = task_id;
  record.attempt = 2;
  record.worker_id = GenerateUuid();
  record.inputs = {"in1"};
  record.outputs = {"out1"};
  record.environment.engine_version = "0.1.0";
  record.environment.git_commit = "abc";
  record.hardware.os = "windows";
  record.started_at_ns = 100;
  record.ended_at_ns = 200;
  record.terminal_state = TaskStatus::kSucceeded;
  record.error = std::nullopt;
  store_->RecordRun(record);

  ErrorRecord error;
  error.code = "WORKER_CRASHED";
  error.message = "boom";
  error.recoverable = true;
  ExecutionRecord failed;
  failed.id = GenerateUuid();
  failed.task_id = task_id;
  failed.attempt = 3;
  failed.terminal_state = TaskStatus::kFailed;
  failed.error = error;
  store_->RecordRun(failed);

  const auto runs = store_->LoadRuns(task_id);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].attempt, 2);
  EXPECT_EQ(runs[0].terminal_state, TaskStatus::kSucceeded);
  EXPECT_EQ(runs[0].outputs.size(), 1u);
  EXPECT_EQ(runs[0].outputs[0], "out1");
  ASSERT_TRUE(runs[0].worker_id.has_value());
  EXPECT_EQ(runs[1].attempt, 3);
  ASSERT_TRUE(runs[1].error.has_value());
  EXPECT_EQ(runs[1].error->code, "WORKER_CRASHED");
  EXPECT_TRUE(runs[1].error->recoverable);
}

TEST_F(SchedulerStateStoreTest, CacheEntryLifecycle) {
  CacheEntryRecord entry;
  entry.cache_key = "key-1";
  entry.artifact_id = GenerateUuid();
  entry.task_type = "feature_extract";
  entry.producer_version = "p1";
  entry.git_commit = "c1";
  entry.config_hash = "cfg";
  store_->UpsertCacheEntry(entry);

  const auto found = store_->FindCacheEntry("key-1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->artifact_id, entry.artifact_id);
  EXPECT_EQ(found->task_type, "feature_extract");

  store_->DeleteCacheEntry("key-1");
  EXPECT_FALSE(store_->FindCacheEntry("key-1").has_value());
}

}  // namespace
}  // namespace spatial::engine
