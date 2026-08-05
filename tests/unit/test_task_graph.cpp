#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/errors/project_error.h"
#include "core/utils/uuid.h"
#include "engine/task/task_graph.h"

#include "tests/unit/engine_test_helpers.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::SchedulerError;

TaskGraph BuildChain(const std::vector<std::string>& types,
                     const std::vector<std::string>& hashes) {
  TaskGraph graph(spatial::core::GenerateUuid());
  std::vector<Uuid> ids;
  for (std::size_t i = 0; i < types.size(); ++i) {
    ids.push_back(graph.AddTask(test::MakeTask(types[i], {}, {hashes[i]})));
  }
  for (std::size_t i = 1; i < ids.size(); ++i) {
    graph.AddDependency(ids[i], ids[i - 1]);
  }
  // Feed the declared outputs into the next task's inputs.
  for (std::size_t i = 1; i < ids.size(); ++i) {
    auto& task = graph.MutableTask(ids[i]);
    task.inputs = {hashes[i - 1]};
  }
  return graph;
}

TEST(TaskGraph, AddAndQuery) {
  TaskGraph graph(spatial::core::GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("a", {}, {"h1"}));
  const auto b = graph.AddTask(test::MakeTask("b", {"h1"}, {"h2"}));
  graph.AddDependency(b, a);

  EXPECT_EQ(graph.size(), 2u);
  EXPECT_TRUE(graph.Contains(a));
  EXPECT_EQ(graph.DependenciesOf(b).size(), 1u);
  EXPECT_EQ(graph.DependenciesOf(b)[0], a);
  EXPECT_EQ(graph.DependentsOf(a).size(), 1u);
  EXPECT_EQ(graph.DependentsOf(a)[0], b);
  EXPECT_EQ(graph.Order().size(), 2u);

  TaskGraph other(spatial::core::GenerateUuid());
  EXPECT_THROW(other.GetTask(a), SchedulerError);
  EXPECT_THROW(graph.AddDependency(a, spatial::core::GenerateUuid()),
               SchedulerError);
}

TEST(TaskGraph, TopologicalOrder) {
  auto graph = BuildChain({"a", "b", "c"}, {"h1", "h2", "h3"});
  const auto order = graph.TopologicalOrder();
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(graph.GetTask(order[0]).definition.type, "a");
  EXPECT_EQ(graph.GetTask(order[1]).definition.type, "b");
  EXPECT_EQ(graph.GetTask(order[2]).definition.type, "c");
}

TEST(TaskGraph, CycleRejected) {
  TaskGraph graph(spatial::core::GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("a"));
  const auto b = graph.AddTask(test::MakeTask("b"));
  graph.AddDependency(a, b);
  graph.AddDependency(b, a);
  EXPECT_THROW(graph.TopologicalOrder(), SchedulerError);
  EXPECT_THROW(graph.Validate({test::BigWorker()}, {}), SchedulerError);
}

TEST(TaskGraph, ValidChainPassesValidation) {
  auto graph = BuildChain({"a", "b", "c"}, {"h1", "h2", "h3"});
  EXPECT_NO_THROW(graph.Validate({test::BigWorker()}, {}));
}

TEST(TaskGraph, InputFromTwoDependenciesIsTypeMismatch) {
  TaskGraph graph(spatial::core::GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("a", {}, {"h1"}));
  const auto b = graph.AddTask(test::MakeTask("b", {}, {"h1"}));
  const auto c = graph.AddTask(test::MakeTask("c", {"h1"}, {"h2"}));
  graph.AddDependency(c, a);
  graph.AddDependency(c, b);
  try {
    graph.Validate({test::BigWorker()}, {});
    FAIL() << "expected SCHED_DAG_TYPE_MISMATCH";
  } catch (const SchedulerError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kSchedDagTypeMismatch);
  }
}

TEST(TaskGraph, UndeclaredExternalInputIsTypeMismatch) {
  TaskGraph graph(spatial::core::GenerateUuid());
  const auto a = graph.AddTask(test::MakeTask("a", {}, {"h1"}));
  const auto b = graph.AddTask(test::MakeTask("b", {"h2"}, {"h3"}));
  graph.AddDependency(b, a);
  try {
    graph.Validate({test::BigWorker()}, {});
    FAIL() << "expected SCHED_DAG_TYPE_MISMATCH";
  } catch (const SchedulerError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kSchedDagTypeMismatch);
  }
  // Declaring the external input makes the same graph valid.
  EXPECT_NO_THROW(graph.Validate({test::BigWorker()}, {"h2"}));
}

TEST(TaskGraph, ResourceFeasibility) {
  TaskGraph graph(spatial::core::GenerateUuid());
  auto task = test::MakeTask("heavy", {}, {"h1"});
  task.definition.requirements.ram_bytes = std::int64_t{1} << 40;
  graph.AddTask(task);
  EXPECT_NO_THROW(graph.Validate({test::BigWorker()}, {}));

  ResourceProfile small;
  small.name = "small-worker";
  small.capacity.cores = 1;
  try {
    graph.Validate({small}, {});
    FAIL() << "expected SCHED_DAG_RESOURCE_INFEASIBLE";
  } catch (const SchedulerError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kSchedDagResourceInfeasible);
  }
}

}  // namespace
}  // namespace spatial::engine
