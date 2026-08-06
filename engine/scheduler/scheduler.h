#pragma once

// Scheduler (RFC-0003 §5.4-§5.8, scheduler-design §2-§6). Owns DAG
// validation, the ready set, cache-first dispatch, retry/cancellation, and
// persisted run state. M0 executes one task at a time on a single
// WorkerExecutor (max_concurrency = 1), which is also the co-running
// resource-feasibility guarantee; concurrency arrives with ProcessExecutor
// (P1.3).

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/engine_common.h"
#include "engine/cache/task_cache.h"
#include "engine/scheduler/scheduler_state_store.h"
#include "engine/task/task_graph.h"
#include "engine/task/task_instance.h"
#include "engine/workers/worker_handle.h"

namespace spatial::engine {

class Scheduler {
 public:
  Scheduler(SchedulerStateStore& state, TaskCache& cache,
            WorkerExecutor& executor);

  // Validates the graph (acyclicity, type-match, resource feasibility),
  // persists it, then executes it to completion. Throws SchedulerError on
  // validation failure.
  void Run(TaskGraph& graph);
  void Run(TaskGraph& graph, const std::vector<ArtifactRef>& external_inputs);

  // Cooperative cancellation of a running task; the terminal cancellation is
  // persisted as a first-class state.
  void Cancel(const Uuid& task_id, const std::string& reason);

  // Resume: reloads a persisted job from project.db and drives the earliest
  // incomplete tasks to completion (scheduler-design §6). `running` tasks are
  // reconciled back to `pending` (their workers died with the host);
  // cancelled tasks stay cancelled and are never re-run (ADR-020).
  void Resume(const Uuid& job_id);

  // Latest persisted state of a task (for status reporting).
  std::optional<TaskInstance> TaskState(const Uuid& task_id) const;

 private:
  void Execute(TaskGraph& graph);
  void PropagateFailures(TaskGraph& graph, bool& progressed);
  // RFC-0003 §5.9: after a task succeeds, its produced output refs replace
  // the declared (placeholder) output refs in each dependent's inputs, so the
  // downstream ADR-020 cache key and ExecutionRecord.inputs reflect real
  // content hashes. `declared` are the refs the dependent consumed before the
  // producer executed. Dependents are persisted on their next state mark.
  void ThreadOutputsToDependents(TaskGraph& graph, Uuid task_id,
                                 const std::vector<ArtifactRef>& declared,
                                 const std::vector<ArtifactRef>& produced);
  std::vector<Uuid> ReadyTasks(const TaskGraph& graph) const;
  bool AllTerminal(const TaskGraph& graph) const;
  bool TryCacheHit(TaskGraph& graph, Uuid task_id);
  void DispatchAndAwait(TaskGraph& graph, Uuid task_id);
  void Mark(TaskGraph& graph, Uuid task_id, TaskStatus to,
            const Uuid& job_id);
  void RecordRun(const TaskInstance& task, TaskStatus terminal,
                 std::vector<ArtifactRef> outputs,
                 std::optional<ErrorRecord> error,
                 std::optional<Uuid> worker_id,
                 std::int64_t started_at_ns);

  SchedulerStateStore& state_;
  TaskCache& cache_;
  WorkerExecutor& executor_;
  TaskGraph* active_graph_ = nullptr;
  std::unique_ptr<TaskGraph> owned_graph_;
};

}  // namespace spatial::engine
