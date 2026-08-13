#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/project/project.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
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
            ("spatial_engine_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
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

}  // namespace
}  // namespace spatial::engine
