#include "engine/engine.h"

#include <utility>

#include "core/errors/project_error.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::Project;
using spatial::core::SchedulerError;

TaskStatus AggregateStatus(const ExecutionPlan& plan, const TaskGraph& graph) {
  bool any_cancelled = false;
  bool any_failed = false;
  for (const auto& stage : plan.stages) {
    switch (graph.GetTask(stage.task_id).state) {
      case TaskStatus::kCancelled:
        any_cancelled = true;
        break;
      case TaskStatus::kSucceeded:
        break;
      default:
        any_failed = true;
        break;
    }
  }
  if (any_cancelled) {
    return TaskStatus::kCancelled;
  }
  if (any_failed) {
    return TaskStatus::kFailed;
  }
  return TaskStatus::kSucceeded;
}

}  // namespace

Engine::Engine(Project project, InProcessTaskRunner runner,
               ResourceProfile profile)
    : project_(std::move(project)),
      state_store_(std::make_unique<SchedulerStateStore>(project_.db())),
      cache_(std::make_unique<TaskCache>(project_.artifacts(), *state_store_,
                                         kEngineVersion, kEngineGitCommit)),
      manifest_store_(std::make_unique<ExecutionManifestStore>(project_.db())),
      compiler_(kEngineVersion, kEngineGitCommit) {
  if (!runner) {
    runner = MakeMockPipelineRunner(project_.artifacts());
  }
  executor_ = std::make_unique<InProcessExecutor>(std::move(profile),
                                                  std::move(runner));
  scheduler_ =
      std::make_unique<Scheduler>(*state_store_, *cache_, *executor_);
}

Engine::~Engine() = default;

ExecutionManifest Engine::RunPipeline(
    const std::string& pipeline_id,
    const std::vector<ArtifactRef>& external_inputs,
    const std::string& config_json) {
  const auto& def = registry_.Resolve(pipeline_id);
  auto plan = compiler_.Compile(def, external_inputs, config_json,
                                executor_->profile(), "inprocess");

  manifest_store_->Begin(plan);
  for (std::size_t i = 0; i < plan.stages.size(); ++i) {
    manifest_store_->InsertStage(plan.job_id, plan.stages[i],
                                 static_cast<int>(i));
  }

  scheduler_->Run(*plan.graph, plan.external_inputs, plan.pipeline_hash);

  // Mirror the terminal per-stage state into the manifest. A stage served
  // from the task cache has no task_runs row (the scheduler records runs for
  // executions only, RFC-0003 §5.10): that absence is the cache-hit signal
  // (AC-8, pipeline_cache).
  for (std::size_t i = 0; i < plan.stages.size(); ++i) {
    const auto& task = plan.graph->GetTask(plan.stages[i].task_id);
    const bool cache_hit = state_store_->LoadRuns(task.id).empty();
    manifest_store_->UpdateStage(plan.job_id, static_cast<int>(i), task.state,
                                 cache_hit, task.outputs,
                                 task.metadata.created_at_ns,
                                 task.metadata.updated_at_ns);
  }
  manifest_store_->Finish(plan.job_id, AggregateStatus(plan, *plan.graph));

  // RFC-0005: link the quality report artifact produced by the terminal
  // validate stage. quality_report_id = artifact_uuid (migration 0004), so
  // `spatial report` and audit consumers can resolve report -> payload.
  if (!plan.stages.empty() &&
      plan.stages.back().capability == "validation") {
    const auto& validate = plan.stages.back();
    const auto& task = plan.graph->GetTask(validate.task_id);
    if (!task.outputs.empty()) {
      const auto row =
          project_.db().FindArtifactByHash(task.outputs.front());
      if (row) {
        manifest_store_->SetQualityReportId(plan.job_id, row->artifact_id);
      }
    }
  }

  auto manifest = manifest_store_->Load(plan.job_id);
  if (!manifest) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "manifest lost after pipeline run");
  }
  return *manifest;
}

Uuid Engine::RunGraph(TaskGraph& graph,
                      const std::vector<ArtifactRef>& external_inputs) {
  scheduler_->Run(graph, external_inputs);
  return graph.job_id();
}

std::optional<ExecutionManifest> Engine::LoadManifest(
    const Uuid& manifest_id) const {
  return manifest_store_->Load(manifest_id);
}

std::vector<Uuid> Engine::ListManifests() const {
  return manifest_store_->ListIds();
}

std::vector<TaskInstance> Engine::LoadJobTasks(const Uuid& job_id) const {
  return state_store_->LoadTasks(job_id);
}

}  // namespace spatial::engine
