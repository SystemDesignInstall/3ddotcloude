#pragma once

// Small shared builders for the engine unit tests (ADR-021: fake/mock
// workers behind the same contract). Not part of the shipped engine.

#include <vector>

#include "core/utils/uuid.h"
#include "engine/resources/resource_spec.h"
#include "engine/task/task_instance.h"

namespace spatial::engine::test {

inline TaskInstance MakeTask(const std::string& type,
                             std::vector<ArtifactRef> inputs = {},
                             std::vector<ArtifactRef> outputs = {}) {
  TaskInstance task;
  task.id = spatial::core::GenerateUuid();
  task.definition.type = type;
  task.definition.inputs.artifact_kinds = {"any"};
  task.definition.outputs.artifact_kinds = {"any"};
  task.definition.requirements.cores = 1;
  task.inputs = std::move(inputs);
  task.outputs = std::move(outputs);
  task.metadata.created_at_ns = 1;
  task.metadata.updated_at_ns = 1;
  return task;
}

// A worker large enough to fit any task used in the unit tests.
inline ResourceProfile BigWorker() {
  ResourceProfile profile;
  profile.name = "big-worker";
  profile.capabilities = {"feature_extraction", "reconstruction"};
  profile.capacity.cores = 64;
  profile.capacity.ram_bytes = std::int64_t{1} << 40;
  profile.capacity.gpus = 8;
  profile.capacity.gpu_mem_bytes = std::int64_t{1} << 40;
  profile.capacity.temp_disk_bytes = std::int64_t{1} << 40;
  profile.max_concurrency = 1;
  return profile;
}

}  // namespace spatial::engine::test
