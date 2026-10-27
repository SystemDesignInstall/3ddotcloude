#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/project/project.h"
#include "core/errors/project_error.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/scheduler/scheduler_state_store.h"
#include "engine/task/task_graph.h"
#include "engine/workers/in_process_executor.h"
#include "tests/unit/engine_test_helpers.h"

namespace spatial::engine {
namespace {

using spatial::core::GenerateUuid;
using spatial::core::Project;
using spatial::core::ProjectInfo;
using spatial::core::fs::Iso8601UtcNow;

class EngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_engine_" +
             spatial::core::FormatUuid(spatial::core::GenerateUuid()));
    project_path_ = root_ / "demo.spx";
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "demo";
    info.created_at = Iso8601UtcNow();
    project_ = std::make_unique<Project>(Project::Create(project_path_, info));
  }

  void TearDown() override {
    project_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
  std::filesystem::path project_path_;
  std::unique_ptr<Project> project_;
};

TEST_F(EngineTest, EngineConstructsAndLoadsEmptyManifestList) {
  printf("step: construct engine\n");
  fflush(stdout);
  Engine engine(std::move(*project_));
  printf("step: list manifests\n");
  fflush(stdout);
  const auto ids = engine.ListManifests();
  printf("step: done, ids=%zu\n", ids.size());
  fflush(stdout);
  EXPECT_TRUE(ids.empty());
}

// C1-S1: an injected WorkerExecutor replaces the default in-process executor
// and the scheduler runs through it end-to-end.
TEST_F(EngineTest, InjectedWorkerExecutorRunsTasks) {
  auto executor = std::make_unique<InProcessExecutor>(test::BigWorker());
  Engine engine(std::move(*project_), std::move(executor));

  TaskGraph graph(GenerateUuid());
  graph.AddTask(test::MakeTask("demo_task", {}, {"out"}));
  const auto job_id = engine.RunGraph(graph, {});

  const auto tasks = engine.LoadJobTasks(job_id);
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_EQ(tasks[0].state, TaskStatus::kSucceeded);
  EXPECT_EQ(tasks[0].outputs.size(), 1u);
  EXPECT_EQ(tasks[0].outputs[0].size(), 64u);
}

TEST_F(EngineTest, PermanentRunnerFailureIsRecordedOnceWithoutRetry) {
  std::atomic<int> calls{0};
  Engine engine(std::move(*project_),
                [&calls](const TaskRequest&, auto, auto) {
                  ++calls;
                  throw spatial::core::ValidationError(
                      spatial::core::ErrorCode::kValidationDomain,
                      "invalid pinned reconstruction input");
                }, test::BigWorker());
  TaskGraph graph(GenerateUuid());
  auto task = test::MakeTask("feature_extract");
  task.metadata.retry.max_attempts = 3;
  task.metadata.retry.base_ns = 0;
  task.metadata.cache = CachePolicy::kNever;
  const auto task_id = graph.AddTask(std::move(task));
  const auto dependent_id = graph.AddTask(test::MakeTask("reconstruct"));
  graph.AddDependency(dependent_id, task_id);

  engine.RunGraph(graph, {});

  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(graph.GetTask(task_id).state, TaskStatus::kFailed);
  EXPECT_EQ(graph.GetTask(dependent_id).state, TaskStatus::kSkipped);
  SchedulerStateStore state(engine.project().db());
  const auto runs = state.LoadRuns(task_id);
  ASSERT_EQ(runs.size(), 1u);
  ASSERT_TRUE(runs[0].error.has_value());
  EXPECT_EQ(runs[0].error->code, "VALIDATION_DOMAIN");
  EXPECT_FALSE(runs[0].error->recoverable);
  EXPECT_TRUE(runs[0].outputs.empty());
  EXPECT_TRUE(state.LoadRuns(dependent_id).empty());
}

TEST_F(EngineTest, TransientRunnerFailureRetriesAndRetainsItsErrorCode) {
  std::atomic<int> calls{0};
  Engine engine(std::move(*project_),
                [&calls](const TaskRequest& request, auto emit, auto cancelled) {
                  if (++calls == 1) {
                    throw spatial::core::StorageError(
                        spatial::core::ErrorCode::kStorageLock,
                        "storage temporarily locked", {}, true);
                  }
                  DefaultInProcessRunner(request, emit, cancelled);
                }, test::BigWorker());
  TaskGraph graph(GenerateUuid());
  auto task = test::MakeTask("feature_extract");
  task.metadata.retry.base_ns = 0;
  task.metadata.cache = CachePolicy::kNever;
  const auto task_id = graph.AddTask(std::move(task));

  engine.RunGraph(graph, {});

  EXPECT_EQ(calls.load(), 2);
  EXPECT_EQ(graph.GetTask(task_id).state, TaskStatus::kSucceeded);
  SchedulerStateStore state(engine.project().db());
  const auto runs = state.LoadRuns(task_id);
  ASSERT_EQ(runs.size(), 2u);
  ASSERT_TRUE(runs[0].error.has_value());
  EXPECT_EQ(runs[0].error->code, "STORAGE_LOCK");
  EXPECT_TRUE(runs[0].error->recoverable);
  EXPECT_EQ(runs[0].terminal_state, TaskStatus::kFailed);
  EXPECT_EQ(runs[1].terminal_state, TaskStatus::kSucceeded);
  EXPECT_FALSE(runs[1].error.has_value());
}

}  // namespace
}  // namespace spatial::engine
