#pragma once

// PipelineCompiler - the ONLY component that builds TaskGraphs (RFC-0003 §5.1,
// ADR-026). Compile() turns a declarative PipelineDefinition into an
// ExecutionPlan: capability resolution against the available worker profile,
// stable placeholder output refs for intra-pipeline data flow (the scheduler
// threads real content hashes at runtime, §5.9), and the identity hash chain
// (pipeline hash -> task hash -> artifact hash). Future planners (hardware /
// distributed / cost / AI / accuracy) compose here without touching the
// scheduler.

#include <string>
#include <vector>

#include "engine/pipeline/execution_plan.h"
#include "engine/pipeline/pipeline_definition.h"
#include "engine/resources/resource_spec.h"

namespace spatial::engine {

class PipelineCompiler {
 public:
  // `producer_version` and `engine_git_commit` feed the ADR-020 cache keys of
  // the compiled tasks (fixed for the engine build).
  PipelineCompiler(std::string producer_version, std::string engine_git_commit);

  // Compiles a pipeline into an ExecutionPlan. Throws ValidationError on
  // empty pipelines / empty external inputs / unknown capabilities; throws
  // SchedulerError from TaskGraph construction on structural problems.
  // `implementation` is the bound worker label (e.g. "inprocess" | "process")
  // recorded in the manifest stage rows.
  ExecutionPlan Compile(const PipelineDefinition& pipeline,
                        const std::vector<ArtifactRef>& external_inputs,
                        const std::string& config_json,
                        const ResourceProfile& worker_profile,
                        const std::string& implementation) const;

  // Pipeline-level identity hash (AC-8): SHA-256 over pipeline id, version,
  // implementation git_commit, config hash, and sorted external input hashes.
  static std::string PipelineHash(const PipelineDefinition& pipeline,
                                  const std::string& config_hash,
                                  const std::vector<ArtifactRef>& external_inputs);

  // SHA-256 of the run configuration (canonical JSON string).
  static std::string ConfigHash(const std::string& config_json);

 private:
  std::string producer_version_;
  std::string engine_git_commit_;
};

}  // namespace spatial::engine
