#include "engine/task/task_graph.h"

#include <algorithm>
#include <deque>
#include <set>
#include <string>
#include <utility>

#include "core/errors/project_error.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::GenerateUuid;
using spatial::core::IsNil;
using spatial::core::SchedulerError;

}  // namespace

Uuid TaskGraph::AddTask(TaskInstance task) {
  if (IsNil(task.id)) {
    task.id = GenerateUuid();
  }
  if (tasks_.count(task.id) != 0) {
    throw SchedulerError(ErrorCode::kSchedTaskUnknown,
                         "duplicate task id in graph: " +
                             core::FormatUuid(task.id));
  }
  tasks_.emplace(task.id, std::move(task));
  dependencies_[task.id] = {};
  dependents_[task.id] = {};
  insertion_order_.push_back(task.id);
  return task.id;
}

void TaskGraph::AddDependency(Uuid task_id, Uuid dependency_id) {
  Require(task_id);
  Require(dependency_id);
  auto& deps = dependencies_[task_id];
  if (std::find(deps.begin(), deps.end(), dependency_id) == deps.end()) {
    deps.push_back(dependency_id);
    dependents_[dependency_id].push_back(task_id);
  }
}

const TaskInstance& TaskGraph::Require(Uuid task_id) const {
  const auto it = tasks_.find(task_id);
  if (it == tasks_.end()) {
    throw SchedulerError(ErrorCode::kSchedTaskUnknown,
                         "unknown task " + core::FormatUuid(task_id));
  }
  return it->second;
}

const TaskInstance& TaskGraph::GetTask(Uuid task_id) const {
  return Require(task_id);
}

TaskInstance& TaskGraph::MutableTask(Uuid task_id) {
  const auto it = tasks_.find(task_id);
  if (it == tasks_.end()) {
    throw SchedulerError(ErrorCode::kSchedTaskUnknown,
                         "unknown task " + core::FormatUuid(task_id));
  }
  return it->second;
}

const std::vector<Uuid>& TaskGraph::DependenciesOf(Uuid task_id) const {
  Require(task_id);
  return dependencies_.at(task_id);
}

std::vector<Uuid> TaskGraph::DependentsOf(Uuid task_id) const {
  Require(task_id);
  return dependents_.at(task_id);
}

std::vector<Uuid> TaskGraph::TaskIds() const { return insertion_order_; }

std::vector<Uuid> TaskGraph::TopologicalOrder() const {
  std::map<Uuid, int> in_degree;
  for (const auto& [id, deps] : dependencies_) {
    in_degree[id] = static_cast<int>(deps.size());
  }
  std::deque<Uuid> ready;
  for (const auto& id : insertion_order_) {
    if (in_degree[id] == 0) {
      ready.push_back(id);
    }
  }
  std::vector<Uuid> order;
  order.reserve(tasks_.size());
  while (!ready.empty()) {
    const Uuid id = ready.front();
    ready.pop_front();
    order.push_back(id);
    for (const auto& dependent : dependents_.at(id)) {
      if (--in_degree[dependent] == 0) {
        ready.push_back(dependent);
      }
    }
  }
  if (order.size() != tasks_.size()) {
    throw SchedulerError(ErrorCode::kSchedDagCycle,
                         "task graph contains a cycle");
  }
  return order;
}

void TaskGraph::Validate(
    const std::vector<ResourceProfile>& worker_pool,
    const std::vector<ArtifactRef>& external_inputs) const {
  // (1) Acyclicity: a topological order exists.
  TopologicalOrder();

  // (2) Type-match: every input is an expected_output of exactly one
  // dependency, or a declared external input.
  const std::set<ArtifactRef> external(external_inputs.begin(),
                                       external_inputs.end());
  for (const auto& id : insertion_order_) {
    // Producers counted per dependency (distinct tasks), per input ref.
    std::map<ArtifactRef, int> producers;
    for (const auto& dep : dependencies_.at(id)) {
      const auto& dep_outputs = tasks_.at(dep).outputs;
      for (const auto& ref : dep_outputs) {
        ++producers[ref];
      }
    }
    const auto& inputs = tasks_.at(id).inputs;
    for (const auto& ref : inputs) {
      const int count = producers.count(ref) ? producers.at(ref) : 0;
      if (count > 1) {
        throw SchedulerError(
            ErrorCode::kSchedDagTypeMismatch,
            "input " + ref + " of task " + core::FormatUuid(id) +
                " is produced by " + std::to_string(count) +
                " dependencies (exactly one required)");
      }
      if (count == 0 && external.count(ref) == 0) {
        throw SchedulerError(
            ErrorCode::kSchedDagTypeMismatch,
            "input " + ref + " of task " + core::FormatUuid(id) +
                " is not produced by any dependency and is not a declared "
                "external input");
      }
    }
  }

  // (3) Resource feasibility: each task fits at least one advertised worker
  // profile. The union-of-co-running check happens at dispatch time.
  for (const auto& id : insertion_order_) {
    const auto& requirements = tasks_.at(id).definition.requirements;
    const bool feasible = std::any_of(
        worker_pool.begin(), worker_pool.end(), [&](const auto& profile) {
          return Fits(requirements, profile.capacity);
        });
    if (!feasible) {
      throw SchedulerError(
          ErrorCode::kSchedDagResourceInfeasible,
          "task " + core::FormatUuid(id) + " (" +
              tasks_.at(id).definition.type +
              ") requirements fit no worker profile");
    }
  }
}

}  // namespace spatial::engine
