#include "engine/scheduler/scheduler.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "engine/scheduler/task_state_machine.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::SchedulerError;
using spatial::core::fs::TimestampNsNow;

}  // namespace

Scheduler::Scheduler(SchedulerStateStore& state, TaskCache& cache,
                     WorkerExecutor& executor)
    : state_(state), cache_(cache), executor_(executor) {}

void Scheduler::Run(TaskGraph& graph) {
  Run(graph, {});
}

void Scheduler::Run(TaskGraph& graph,
                    const std::vector<ArtifactRef>& external_inputs) {
  graph.Validate({executor_.profile()}, external_inputs);
  state_.SaveGraph(graph);
  Execute(graph);
}

void Scheduler::Resume(const Uuid& job_id) {
  owned_graph_ = std::make_unique<TaskGraph>(job_id);
  for (auto task : state_.LoadTasks(job_id)) {
    // Reconcile: `running` tasks were interrupted by the host restart and
    // are re-run from scratch (scheduler-design §6). Cancelled tasks stay
    // cancelled and are never re-run (ADR-020).
    if (task.state == TaskStatus::kRunning) {
      task.state = TaskStatus::kPending;
      task.metadata.attempts = 0;
    }
    owned_graph_->AddTask(std::move(task));
  }
  for (const auto& [task_id, dep] : state_.LoadDependencies(job_id)) {
    owned_graph_->AddDependency(task_id, dep);
  }
  Execute(*owned_graph_);
}

void Scheduler::Cancel(const Uuid& task_id, const std::string& reason) {
  if (active_graph_ == nullptr) {
    return;
  }
  const auto task = active_graph_->GetTask(task_id);
  switch (task.state) {
    case TaskStatus::kPending:
      Mark(*active_graph_, task_id, TaskStatus::kCancelled,
           active_graph_->job_id());
      return;
    case TaskStatus::kRunning:
      // Cooperative: the worker stops at its next safe checkpoint and the
      // terminal cancellation is handled by the event loop.
      executor_.Cancel(task_id, reason);
      return;
    default:
      return;  // already terminal
  }
}

std::optional<TaskInstance> Scheduler::TaskState(const Uuid& task_id) const {
  return state_.FindTask(task_id);
}

void Scheduler::Execute(TaskGraph& graph) {
  active_graph_ = &graph;
  for (int iteration = 0; iteration < 10000; ++iteration) {
    bool progressed = false;
    PropagateFailures(graph, progressed);
    if (AllTerminal(graph)) {
      break;
    }

    for (const auto& id : ReadyTasks(graph)) {
      if (TryCacheHit(graph, id)) {
        progressed = true;
      }
    }
    for (const auto& id : ReadyTasks(graph)) {
      DispatchAndAwait(graph, id);
      progressed = true;
    }
    if (!progressed) {
      throw SchedulerError(ErrorCode::kInternal,
                           "scheduler stalled: no task is ready and the graph "
                           "is not terminal");
    }
  }
  active_graph_ = nullptr;
}

void Scheduler::PropagateFailures(TaskGraph& graph, bool& progressed) {
  for (const auto& id : graph.Order()) {
    const auto& task = graph.GetTask(id);
    if (task.state != TaskStatus::kPending) {
      continue;
    }
    bool dep_failed = false;
    for (const auto& dep : graph.DependenciesOf(id)) {
      const auto dep_state = graph.GetTask(dep).state;
      if (IsTerminal(dep_state) && dep_state != TaskStatus::kSucceeded) {
        dep_failed = true;
        break;
      }
    }
    if (dep_failed) {
      Mark(graph, id,
           task.metadata.failure == FailurePolicy::kFailed
               ? TaskStatus::kFailed
               : TaskStatus::kSkipped,
           graph.job_id());
      progressed = true;
    }
  }
}

std::vector<Uuid> Scheduler::ReadyTasks(const TaskGraph& graph) const {
  std::vector<Uuid> ready;
  for (const auto& id : graph.Order()) {
    const auto& task = graph.GetTask(id);
    if (task.state != TaskStatus::kPending) {
      continue;
    }
    bool deps_succeeded = true;
    for (const auto& dep : graph.DependenciesOf(id)) {
      if (graph.GetTask(dep).state != TaskStatus::kSucceeded) {
        deps_succeeded = false;
        break;
      }
    }
    if (deps_succeeded) {
      ready.push_back(id);
    }
  }
  return ready;
}

bool Scheduler::AllTerminal(const TaskGraph& graph) const {
  for (const auto& id : graph.Order()) {
    if (!IsTerminal(graph.GetTask(id).state)) {
      return false;
    }
  }
  return true;
}

bool Scheduler::TryCacheHit(TaskGraph& graph, Uuid task_id) {
  const auto& task = graph.GetTask(task_id);
  const auto entry = cache_.Lookup(task);
  if (!entry) {
    return false;
  }
  const auto output = cache_.ResolveOutput(*entry);
  if (!output) {
    cache_.Invalidate(entry->cache_key);
    return false;
  }
  // pending -> running -> succeeded (the task is satisfied without dispatch).
  Mark(graph, task_id, TaskStatus::kRunning, graph.job_id());
  auto& mutable_task = graph.MutableTask(task_id);
  mutable_task.outputs = {*output};
  Mark(graph, task_id, TaskStatus::kSucceeded, graph.job_id());
  RecordRun(mutable_task, TaskStatus::kSucceeded, mutable_task.outputs,
            std::nullopt, std::nullopt, TimestampNsNow() - 1);
  return true;
}

void Scheduler::DispatchAndAwait(TaskGraph& graph, Uuid task_id) {
  const Uuid job_id = graph.job_id();
  for (;;) {
    auto& task = graph.MutableTask(task_id);
    if (task.state != TaskStatus::kRunning) {
      Mark(graph, task_id, TaskStatus::kRunning, job_id);
    }
    const std::int64_t started_ns = TimestampNsNow();

    TaskRequest request;
    request.task_id = task_id;
    request.task_type = task.definition.type;
    request.input_refs = task.inputs;
    request.expected_output_refs = task.outputs;
    request.config_json = task.config_json;
    request.workspace = "temp/" + FormatUuid(job_id) + "/" + FormatUuid(task_id);
    executor_.Submit(request);

    std::vector<ArtifactRef> produced;
    WorkerEvent event;
    int missed_heartbeats = 0;
    for (bool retry = false; !retry;) {
      if (!executor_.WaitForEvent(event, 5000)) {
        if (++missed_heartbeats > 10) {
          // No heartbeats at all: treat as a worker crash (recoverable, so
          // the retry policy applies). ProcessExecutor surfaces timeouts as
          // WORKER_HEARTBEAT_TIMEOUT; this is the M0 fallback.
          event.type = WorkerEventType::kFailed;
          event.task_id = task_id;
          event.error_code = "WORKER_CRASHED";
          event.error_message = "worker stopped responding";
          event.recoverable = true;
          missed_heartbeats = 0;
        } else {
          continue;
        }
      }
      if (event.task_id != task_id) {
        continue;
      }
      switch (event.type) {
        case WorkerEventType::kArtifactProduced:
          produced.push_back(event.artifact_ref);
          continue;
        case WorkerEventType::kCompleted: {
          auto& t = graph.MutableTask(task_id);
          t.outputs = std::move(produced);
          Mark(graph, task_id, TaskStatus::kSucceeded, job_id);
          if (!t.outputs.empty()) {
            cache_.StoreOutput(t, t.outputs.front());
          }
          RecordRun(t, TaskStatus::kSucceeded, t.outputs,
                    std::nullopt, executor_.id(), started_ns);
          return;
        }
        case WorkerEventType::kFailed: {
          auto& t = graph.MutableTask(task_id);
          ++t.metadata.attempts;
          const bool retryable = event.recoverable &&
                                 t.metadata.attempts <
                                     t.metadata.retry.max_attempts;
          if (retryable) {
            RecordRun(t, TaskStatus::kFailed, {}, {ErrorRecord{
                                                       event.error_code,
                                                       event.error_message,
                                                       true}},
                      executor_.id(), started_ns);
            // running -> failed (run recorded), then failed -> pending
            // (retry edge, gated externally by `retryable`). The sentinel
            // exits the event loop so the outer loop re-dispatches; a plain
            // `break` would only leave the switch and misconsume the next
            // queued event as if this attempt had produced it.
            Mark(graph, task_id, TaskStatus::kFailed, job_id);
            Mark(graph, task_id, TaskStatus::kPending, job_id);
            retry = true;
            break;  // exit the switch (the sentinel exits the event loop)
          } else {
            RecordRun(t, TaskStatus::kFailed, {},
                      {ErrorRecord{event.error_code, event.error_message,
                                   event.recoverable}},
                      executor_.id(), started_ns);
            Mark(graph, task_id, TaskStatus::kFailed, job_id);
            return;
          }
        }
        case WorkerEventType::kCancelled: {
          auto& t = graph.MutableTask(task_id);
          t.outputs = {};  // partial outputs are discarded (ADR-010)
          Mark(graph, task_id, TaskStatus::kCancelled, job_id);
          RecordRun(t, TaskStatus::kCancelled, {}, std::nullopt,
                    executor_.id(), started_ns);
          return;
        }
        default:
          continue;  // progress / log / heartbeat
      }
    }
  }
}

void Scheduler::Mark(TaskGraph& graph, Uuid task_id, TaskStatus to,
                     const Uuid& job_id) {
  auto& task = graph.MutableTask(task_id);
  TaskStateMachine::Transition(task.state, to);
  task.state = to;
  task.metadata.updated_at_ns = TimestampNsNow();
  state_.UpsertTask(job_id, task);
}

void Scheduler::RecordRun(const TaskInstance& task, TaskStatus terminal,
                          std::vector<ArtifactRef> outputs,
                          std::optional<ErrorRecord> error,
                          std::optional<Uuid> worker_id,
                          std::int64_t started_at_ns) {
  ExecutionRecord record;
  record.id = GenerateUuid();
  record.task_id = task.id;
  record.attempt = task.metadata.attempts + 1;
  record.worker_id = std::move(worker_id);
  record.inputs = task.inputs;
  record.outputs = std::move(outputs);
  record.environment.engine_version = kEngineVersion;
  record.environment.git_commit = kEngineGitCommit;
  record.environment.protocol_version = kWorkerProtocolVersion;
  record.hardware.os = "unknown";
  record.hardware.arch = "unknown";
  record.started_at_ns = started_at_ns;
  record.ended_at_ns = TimestampNsNow();
  record.terminal_state = terminal;
  record.error = std::move(error);
  state_.RecordRun(record);
}

}  // namespace spatial::engine
