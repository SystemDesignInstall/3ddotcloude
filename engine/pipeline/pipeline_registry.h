#pragma once

// PipelineRegistry - the dictionary of PipelineDefinitions known to the
// engine (RFC-0003 §5.1). Composition roots (the CLI, tests, an SDK) register
// pipeline implementations here; PipelineCompiler resolves by id and compiles
// to an ExecutionPlan. The registry holds definitions only, never graphs.

#include <map>
#include <string>
#include <vector>

#include "engine/pipeline/pipeline_definition.h"

namespace spatial::engine {

class PipelineRegistry {
 public:
  // Registers a pipeline; duplicate ids are rejected (kValidationDomain).
  void Register(PipelineDefinition pipeline);

  // Resolves by id; throws ValidationError on unknown pipeline.
  const PipelineDefinition& Resolve(const std::string& id) const;

  bool Contains(const std::string& id) const;

  std::vector<std::string> Ids() const;

 private:
  std::map<std::string, PipelineDefinition> pipelines_;
};

}  // namespace spatial::engine
