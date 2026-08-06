#include "engine/pipeline/pipeline_registry.h"

#include <utility>

#include "core/errors/project_error.h"

namespace spatial::engine {

void PipelineRegistry::Register(PipelineDefinition pipeline) {
  if (pipeline.id.empty()) {
    throw core::ValidationError(core::ErrorCode::kValidationDomain,
                                "pipeline id must not be empty");
  }
  const auto [it, inserted] =
      pipelines_.emplace(pipeline.id, std::move(pipeline));
  if (!inserted) {
    throw core::ValidationError(
        core::ErrorCode::kValidationDomain,
        "pipeline already registered: " + pipeline.id);
  }
}

const PipelineDefinition& PipelineRegistry::Resolve(const std::string& id) const {
  const auto it = pipelines_.find(id);
  if (it == pipelines_.end()) {
    throw core::ValidationError(core::ErrorCode::kValidationDomain,
                                "unknown pipeline: " + id);
  }
  return it->second;
}

bool PipelineRegistry::Contains(const std::string& id) const {
  return pipelines_.count(id) == 1;
}

std::vector<std::string> PipelineRegistry::Ids() const {
  std::vector<std::string> out;
  out.reserve(pipelines_.size());
  for (const auto& [id, def] : pipelines_) {
    out.push_back(id);
  }
  return out;
}

}  // namespace spatial::engine
