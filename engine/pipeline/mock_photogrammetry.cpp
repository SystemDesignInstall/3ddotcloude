#include "engine/pipeline/mock_photogrammetry.h"

#include <utility>

#include "engine_build_info.h"

namespace spatial::engine {

void RegisterMockPhotogrammetry(PipelineRegistry& registry) {
  PipelineDefinition def;
  def.id = kMockPhotogrammetryPipelineId;
  def.name = "Mock Photogrammetry";
  def.version = "0.1.0";
  def.git_commit = kEngineGitCommit;
  def.config_schema_json = "{}";
  def.stages = {
      {"feature_extract", "feature_extraction", "feature_extract",
       {"image"}, {"keypoints"}},
      {"reconstruct", "reconstruction", "reconstruct",
       {"keypoints"}, {"point_cloud"}},
      {"validate", "validation", "validate",
       {"point_cloud"}, {"quality_report"}},
  };
  registry.Register(std::move(def));
}

}  // namespace spatial::engine
