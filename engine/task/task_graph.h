#pragma once

// TaskGraph - the DAG handed to the scheduler (RFC-0003 §5.3, task-model §2).
// Nodes are TaskInstances, edges are dependencies. The graph is validated
// once before dispatch: acyclicity, type-match, resource feasibility.

#include <cstddef>
#include <map>
#include <vector>

#include "engine/engine_common.h"
#include "engine/resources/resource_spec.h"
#include "engine/task/task_instance.h"

namespace spatial::engine {

class TaskGraph {
 public:
  explicit TaskGraph(Uuid job_id) : job_id_(job_id) {}

  Uuid job_id() const { return job_id_; }
  std::size_t size() const { return tasks_.size(); }
  bool Contains(Uuid task_id) const { return tasks_.count(task_id) == 1; }
  const std::vector<Uuid>& Order() const { return insertion_order_; }

  // Adds a task; generates a UUID when `task.id` is nil. Returns the id.
  Uuid AddTask(TaskInstance task);

  // Adds an edge dependency_id -> task_id. Both must already exist
  // (SCHED_TASK_UNKNOWN otherwise). A self-loop is rejected by acyclicity
  // validation.
  void AddDependency(Uuid task_id, Uuid dependency_id);

  const TaskInstance& GetTask(Uuid task_id) const;  // SCHED_TASK_UNKNOWN
  // Mutable access for the scheduler (state transitions are persisted by the
  // state store, never by the graph itself).
  TaskInstance& MutableTask(Uuid task_id);
  const std::vector<Uuid>& DependenciesOf(Uuid task_id) const;
  std::vector<Uuid> DependentsOf(Uuid task_id) const;
  std::vector<Uuid> TaskIds() const;  // insertion order

  // Topological order (Kahn). Throws SCHED_DAG_CYCLE when the graph is
  // cyclic.
  std::vector<Uuid> TopologicalOrder() const;

  // Validates the graph once before dispatch (task-model §2). Throws
  // SchedulerError on violation:
  //   SCHED_DAG_CYCLE               graph is not a DAG
  //   SCHED_DAG_TYPE_MISMATCH       an input is produced by more than one
  //                                 dependency, or by none and is not a
  //                                 declared external input
  //   SCHED_DAG_RESOURCE_INFEASIBLE no worker profile fits a task's needs
  void Validate(const std::vector<ResourceProfile>& worker_pool,
                const std::vector<ArtifactRef>& external_inputs) const;

 private:
  const TaskInstance& Require(Uuid task_id) const;

  Uuid job_id_;
  std::map<Uuid, TaskInstance> tasks_;                 // stable key ordering
  std::map<Uuid, std::vector<Uuid>> dependencies_;     // task -> dependencies
  std::map<Uuid, std::vector<Uuid>> dependents_;       // task -> dependents
  std::vector<Uuid> insertion_order_;
};

}  // namespace spatial::engine
