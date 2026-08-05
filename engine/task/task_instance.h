#pragma once

// TaskInstance - one concrete run of a TaskDefinition (RFC-0003 §5.2,
// task-model.md §1). The id is immutable; inputs and outputs are
// content-addressed refs (never paths); the configuration is the effective
// JSON the worker executes, hashed for the ADR-020 cache key.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/engine_common.h"
#include "engine/task/task_definition.h"
#include "engine/task/task_types.h"

namespace spatial::engine {

struct TaskInstance {
  Uuid id{};  // immutable
  TaskDefinition definition;
  std::vector<ArtifactRef> inputs;   // CAS SHA-256 content hashes
  std::vector<ArtifactRef> outputs;  // declared outputs (produced refs)
  std::string config_json;           // effective configuration (JSON)
  TaskStatus state = TaskStatus::kPending;
  TaskMetadata metadata;
};

}  // namespace spatial::engine
