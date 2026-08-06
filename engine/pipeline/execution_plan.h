#pragma once

// ExecutionPlan - the compiled result of PipelineCompiler::Compile
// (RFC-0003 §5.1, ADR-026). A plan binds every pipeline stage to a concrete
// task and a worker implementation by capability, carries the identity hashes
// (pipeline hash / task hash / artifact hash chain), and owns the ONLY
// TaskGraph in the system. No other component builds graphs.

#include <memory>
#include <string>
#include <vector>

#include "engine/engine_common.h"
#include "engine/task/task_graph.h"

namespace spatial::engine {

// One compiled stage: the mapping from a pipeline stage to a concrete task
// bound to a worker implementation (capability resolution, ADR-011).
struct PlanStage {
  std::string stage_id;
  std::string capability;
  std::string implementation;   // "inprocess" | "process"
  std::string task_type;
  std::string config_hash;      // SHA-256 of the effective stage configuration
  std::string task_hash;        // ADR-020 cache key of the compiled task
  Uuid task_id{};
};

struct ExecutionPlan {
  std::string pipeline_id;
  std::string pipeline_version;
  std::string git_commit;
  std::string pipeline_hash;    // identity of the whole run (AC-8)
  std::string config_hash;      // SHA-256 of the run configuration
  std::vector<ArtifactRef> external_inputs;
  std::vector<PlanStage> stages;
  Uuid job_id{};                // manifest_id == graph job_id
  std::int64_t created_at_ns = 0;
  // The only TaskGraph builder in the platform is PipelineCompiler.
  std::unique_ptr<TaskGraph> graph;
};

}  // namespace spatial::engine
