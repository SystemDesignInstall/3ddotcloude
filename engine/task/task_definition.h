#pragma once

// TaskDefinition - the reusable description of an operation (RFC-0003 §5.2).
// Worker selection is by capability (ADR-011/034), never by task name, so
// `type` is a semantic string that maps to capabilities via the frozen
// taxonomy, not an executable name.

#include <string>
#include <vector>

#include "engine/resources/resource_spec.h"

namespace spatial::engine {

// Declared input artifact kinds; the DAG type-match rule (task-model §2)
// checks every actual input ref against exactly one dependency output or a
// declared external input.
struct InputSchema {
  std::vector<std::string> artifact_kinds;  // e.g. "image", "point_cloud"
  bool allow_external = false;  // may be fed by a declared external input
};

// Declared output artifact kinds the worker must produce.
struct OutputSchema {
  std::vector<std::string> artifact_kinds;
};

struct TaskDefinition {
  std::string type;
  InputSchema inputs;
  OutputSchema outputs;
  std::string parameters_schema_json;  // JSON Schema for the configuration
  ResourceSpec requirements;
};

}  // namespace spatial::engine
