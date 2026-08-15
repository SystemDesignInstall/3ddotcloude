#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"
#include "engine/cache/task_cache.h"
#include "engine/scheduler/scheduler.h"
#include "engine/scheduler/scheduler_state_store.h"
#include "engine/workers/worker_handle.h"

#include "tests/unit/engine_test_helpers.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactStore;
using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;

// ADR-021: fake/mock worker behind the same WorkerExecutor contract. The
// script drives the outcome for each submitted task id.
class FakeExecutor : public WorkerExecutor {
 public:
  struct Step {
    WorkerEventType type = WorkerEventType::kCompleted;
    std::string artifact;
    std::string code;
    std::string message;
    bool recoverable = false;
  };

  explicit FakeExecutor(ResourceProfile profile = test::BigWorker())
      : profile_(std::move(profile)) {}

  const ResourceProfile& profile() const override { return profile_; }
  Uuid id() const override { return id_; }
  std::string implementation_label() const override { return "fake"; }

  void Submit(const TaskRequest& request) override {
    submitted_.push_back(request.task_id);
    PushScript(request.task_id);
  }

  void Cancel(const Uuid& task_id, const std::string& reason) override {
    cancelled_.push_back({task_id, reason});
    PushScript(task_id);
  }

  bool WaitForEvent(WorkerEvent& out, std::int64_t /*timeout_ms*/) override {
    if (events_.empty()) {
      return false;
    }
    out = events_.front();
    events_.pop();
    return true;
  }

  void Shutdown() override {}

  void Script(const Uuid& task_id, std::vector<Step> steps) {
    script_[task_id] = std::move(steps);
  }

  std::vector<Uuid> submitted_;
  std::vector<std::pair<Uuid, std::string>> cancelled_;

 private:
  void PushScript(const Uuid& task_id) {
    const auto it = script_.find(task_id);
    if (it == script_.end()) {
      return;
    }
    for (const auto& step : it->second) {
      WorkerEvent event;
      event.type = step.type;
      event.task_id = task_id;
      event.artifact_ref = step.artifact;
      event.error_code = step.code;
      event.error_message = step.message;
      event.recoverable = step.recoverable;
      events_.push(event);
    }
  }

  ResourceProfile profile_;
  Uuid id_ = GenerateUuid();
  std::map<Uuid, std::vector<Step>> script_;
  std::queue<WorkerEvent> events_;
};

class SchedulerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_sched_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = std::make_unique<MetadataDb>(MetadataDb::Create(root_ / "project.db"));
    artifact_store_ =
        std::make_unique<ArtifactStore>(root_ / "artifacts", *db_);
    state_ = std::make_unique<SchedulerStateStore>(*db_);
    cache_ = std::make_unique<TaskCache>(*artifact_store_, *state_,
                                         "producer-v1", "commit-abc");
  }

  void TearDown() override {
    cache_.reset();
    state_.reset();
    artifact_store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
  std::unique_ptr<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> artifact_store_;
  std::unique_ptr<SchedulerStateStore> state_;
  std::unique_ptr<TaskCache> cache_;
};

TEST_F(SchedulerTest, RunsInTopologicalOrder) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  TaskGraph graph(GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("feature_extract", {}, {"h1"}));
  const auto b = graph.AddTask(test::MakeTask("reconstruct", {"h1"}, {"h2"}));
  const auto c = graph.AddTask(test::MakeTask("export", {"h2"}, {"h3"}));
  graph.AddDependency(b, a);
  graph.AddDependency(c, b);
  executor.Script(a, {{.type = WorkerEventType::kCompleted}});
  executor.Script(b, {{.type = WorkerEventType::kCompleted}});
  executor.Script(c, {{.type = WorkerEventType::kCompleted}});

  scheduler.Run(graph);

  ASSERT_EQ(executor.submitted_.size(), 3u);
  EXPECT_EQ(executor.submitted_[0], a);
  EXPECT_EQ(executor.submitted_[1], b);
  EXPECT_EQ(executor.submitted_[2], c);
  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kSucceeded);
  EXPECT_EQ(scheduler.TaskState(b)->state, TaskStatus::kSucceeded);
  EXPECT_EQ(scheduler.TaskState(c)->state, TaskStatus::kSucceeded);
}

TEST_F(SchedulerTest, FailedDependencySkipsDependents) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  TaskGraph graph(GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("feature_extract", {}, {"h1"}));
  const auto b = graph.AddTask(test::MakeTask("reconstruct", {"h1"}, {"h2"}));
  graph.AddDependency(b, a);
  executor.Script(a, {{.type = WorkerEventType::kFailed,
                       .code = "WORKER_CRASHED",
                       .message = "boom",
                       .recoverable = false}});

  scheduler.Run(graph);

  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kFailed);
  EXPECT_EQ(scheduler.TaskState(b)->state, TaskStatus::kSkipped);
  ASSERT_EQ(executor.submitted_.size(), 1u);
  EXPECT_EQ(executor.submitted_[0], a);
}

TEST_F(SchedulerTest, StrictFailurePolicyFailsDependents) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  TaskGraph graph(GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("feature_extract", {}, {"h1"}));
  auto b = test::MakeTask("reconstruct", {"h1"}, {"h2"});
  b.metadata.failure = FailurePolicy::kFailed;
  const auto b_id = graph.AddTask(b);
  graph.AddDependency(b_id, a);
  executor.Script(a, {{.type = WorkerEventType::kFailed,
                       .code = "WORKER_CRASHED",
                       .recoverable = false}});

  scheduler.Run(graph);

  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kFailed);
  EXPECT_EQ(scheduler.TaskState(b_id)->state, TaskStatus::kFailed);
}

TEST_F(SchedulerTest, RecoverableFailureRetriesThenSucceeds) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  TaskGraph graph(GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("feature_extract", {}, {"h1"}));
  executor.Script(a, {{.type = WorkerEventType::kFailed,
                       .code = "WORKER_CRASHED",
                       .recoverable = true},
                      {.type = WorkerEventType::kCompleted}});

  scheduler.Run(graph);

  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kSucceeded);
  ASSERT_EQ(executor.submitted_.size(), 2u);
  EXPECT_EQ(scheduler.TaskState(a)->metadata.attempts, 1);
}

TEST_F(SchedulerTest, RetryExhaustionDeclaresFailure) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  TaskGraph graph(GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("feature_extract", {}, {"h1"}));
  executor.Script(a, {{.type = WorkerEventType::kFailed,
                       .code = "WORKER_CRASHED",
                       .recoverable = true},
                      {.type = WorkerEventType::kFailed,
                       .code = "WORKER_CRASHED",
                       .recoverable = true}});

  scheduler.Run(graph);

  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kFailed);
  EXPECT_EQ(scheduler.TaskState(a)->metadata.attempts, 2);
}

TEST_F(SchedulerTest, PermanentFailureNeverRetries) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  TaskGraph graph(GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("feature_extract", {}, {"h1"}));
  executor.Script(a, {{.type = WorkerEventType::kFailed,
                       .code = "VALIDATION_ERROR",
                       .recoverable = false}});

  scheduler.Run(graph);

  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kFailed);
  ASSERT_EQ(executor.submitted_.size(), 1u);
}

TEST_F(SchedulerTest, CancellationIsFirstClassState) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  TaskGraph graph(GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("feature_extract", {}, {"h1"}));
  executor.Script(a, {{.type = WorkerEventType::kCancelled}});

  scheduler.Run(graph);

  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kCancelled);
  // Cancelled tasks persist as cancelled (never re-run on resume).
  scheduler.Resume(graph.job_id());
  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kCancelled);
}

TEST_F(SchedulerTest, CacheHitSkipsExecution) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  const std::string bytes = "point-bytes";
  const std::vector<std::uint8_t> payload(bytes.begin(), bytes.end());
  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "feature_points";
  manifest.producer = {"engine", "0.1.0", "commit-abc"};
  manifest.creation_timestamp = "2026-01-01T00:00:00Z";
  manifest.file_size = static_cast<std::int64_t>(payload.size());
  manifest.mime_type = "application/octet-stream";
  const auto written = artifact_store_->Put(payload, manifest);

  auto task = test::MakeTask("feature_extract", {}, {written.content_hash});
  task.metadata.deterministic = true;
  task.config_json = R"({"threshold": 0.5})";
  const auto task_id = [&] {
    TaskGraph first(GenerateUuid());
    const auto id = first.AddTask(task);
    executor.Script(id, {{.type = WorkerEventType::kArtifactProduced,
                          .artifact = written.content_hash},
                         {.type = WorkerEventType::kCompleted}});
    scheduler.Run(first);
    return id;
  }();
  ASSERT_EQ(executor.submitted_.size(), 1u);

  // Second run with an unchanged key must be served from cache: the cached
  // task id must not be submitted again. A fresh task id (same type, inputs,
  // config, producer) maps to the same ADR-020 cache key.
  auto task2 = task;
  task2.id = GenerateUuid();
  TaskGraph second(GenerateUuid());
  second.AddTask(task2);
  scheduler.Run(second);
  EXPECT_EQ(executor.submitted_.size(), 1u);
  EXPECT_EQ(scheduler.TaskState(task_id)->outputs[0], written.content_hash);
}

TEST_F(SchedulerTest, ResumeReconcilesRunningTasks) {
  FakeExecutor executor;
  Scheduler scheduler(*state_, *cache_, executor);

  const Uuid job_id = GenerateUuid();
  TaskGraph graph(job_id);
  const auto a = graph.AddTask(test::MakeTask("feature_extract", {}, {"h1"}));
  const auto b = graph.AddTask(test::MakeTask("reconstruct", {"h1"}, {"h2"}));
  graph.AddDependency(b, a);
  state_->SaveGraph(graph);

  // Host restart: a finished, b was running when the host died.
  auto a_task = state_->FindTask(a);
  auto b_task = state_->FindTask(b);
  a_task->state = TaskStatus::kSucceeded;
  b_task->state = TaskStatus::kRunning;
  state_->UpsertTask(job_id, *a_task);
  state_->UpsertTask(job_id, *b_task);
  executor.Script(b, {{.type = WorkerEventType::kCompleted}});

  scheduler.Resume(job_id);

  ASSERT_EQ(executor.submitted_.size(), 1u);
  EXPECT_EQ(executor.submitted_[0], b);
  EXPECT_EQ(scheduler.TaskState(a)->state, TaskStatus::kSucceeded);
  EXPECT_EQ(scheduler.TaskState(b)->state, TaskStatus::kSucceeded);
}

}  // namespace
}  // namespace spatial::engine
