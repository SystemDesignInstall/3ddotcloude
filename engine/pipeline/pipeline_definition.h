#pragma once

// PipelineDefinition - the declarative document of a processing pipeline
// (RFC-0003 §5.1, ADR-026). It describes WHAT to run and in WHICH order, and
// carries the configuration schema; it never builds a TaskGraph (that is
// exclusively PipelineCompiler). Stages form a sequential chain: stage i
// consumes the output of stage i-1.

#include <string>
#include <vector>

namespace spatial::engine {

// One stage of a pipeline. Capability-based (ADR-011/034): the compiler binds
// a worker implementation by `capability`, never by task name.
struct PipelineStage {
  std::string id;                         // e.g. "feature_extract"
  std::string capability;                 // worker-capabilities.schema.json
  std::string task_type;                  // semantic task type the worker runs
  std::vector<std::string> input_artifact_kinds;   // consumed artifact kinds
  std::vector<std::string> output_artifact_kinds;  // produced artifact kinds
};

struct PipelineDefinition {
  std::string id;                // unique registry key
  std::string name;
  std::string version;
  std::string git_commit;        // implementation commit (reproducibility)
  std::string config_schema_json;  // JSON Schema for the run configuration
  std::vector<PipelineStage> stages;  // sequential; the last is the terminal
};

}  // namespace spatial::engine
