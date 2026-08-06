#pragma once

// Engine - the P1.4 composition root (RFC-0003). Owns one open project and
// the components that run against it through the single MetadataDb connection
// (ADR-020: one SQLite writer): scheduler state, task cache, manifest store,
// pipeline registry, and the PipelineCompiler (the ONLY TaskGraph builder).
// It runs the in-process mock implementation by default (ADR-021); callers
// (CLI, tests, a future GUI) inject their own InProcessTaskRunner or profile.
// The Engine is the whole public execution surface of the platform: the CLI is
// just a client of it.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/project/project.h"
#include "engine/engine_common.h"
#include "engine/cache/task_cache.h"
#include "engine/pipeline/execution_manifest.h"
#include "engine/pipeline/pipeline_compiler.h"
#include "engine/pipeline/pipeline_registry.h"
#include "engine/scheduler/scheduler.h"
#include "engine/scheduler/scheduler_state_store.h"
#include "engine/task/task_graph.h"
#include "engine/workers/in_process_executor.h"
#include "engine/workers/mock_pipeline_runner.h"

namespace spatial::engine {

class Engine {
 public:
  // `project` owns the metadata db + artifact store shared by every component
  // (destroyed last). When `runner` is empty the default mock pipeline runner
  // is installed (engine/workers/mock_pipeline_runner).
  Engine(spatial::core::Project project, InProcessTaskRunner runner = {},
         ResourceProfile profile = DemoWorkerProfile());

  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;

  // Compiles and runs a registered pipeline to completion, persisting its
  // ExecutionManifest (migration 0004, RFC-0003 §5.1). Returns the terminal
  // manifest; throws ValidationError/SchedulerError on invalid input.
  ExecutionManifest RunPipeline(const std::string& pipeline_id,
                                const std::vector<ArtifactRef>& external_inputs,
                                const std::string& config_json);

  // Runs a directly-specified TaskGraph (AC-7: `spatial run --dag`). No
  // pipeline manifest is produced for ad hoc DAGs; the graph and its run
  // records are persisted by the scheduler. Returns the job id.
  Uuid RunGraph(TaskGraph& graph,
                const std::vector<ArtifactRef>& external_inputs);

  std::optional<ExecutionManifest> LoadManifest(const Uuid& manifest_id) const;
  std::vector<Uuid> ListManifests() const;
  std::vector<TaskInstance> LoadJobTasks(const Uuid& job_id) const;

  PipelineRegistry& registry() { return registry_; }
  spatial::core::Project& project() { return project_; }

 private:
  spatial::core::Project project_;  // destroyed last (owns db + artifacts)
  std::unique_ptr<SchedulerStateStore> state_store_;
  std::unique_ptr<TaskCache> cache_;
  std::unique_ptr<ExecutionManifestStore> manifest_store_;
  PipelineCompiler compiler_;
  PipelineRegistry registry_;
  std::unique_ptr<InProcessExecutor> executor_;
  std::unique_ptr<Scheduler> scheduler_;
};

}  // namespace spatial::engine
