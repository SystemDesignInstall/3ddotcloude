#pragma once

// Scene and SceneVersion (RFC-0002 §6.1, ADR-033, scene.schema.json).
// A Scene is the central domain object (ADR-023); a SceneVersion is the
// immutable record of one state of the scene (version chain, append-only,
// acyclic). Import produces a new version at stage "imported".

#include <cstdint>
#include <string>

#include "core/utils/uuid.h"

namespace spatial::core {

struct Scene {
  Uuid scene_id{};
  std::int64_t schema_version = 1;
  Uuid project_id{};
  std::string name;
  Uuid version_id{};            // current version (ADR-033)
  Uuid parent_version_id{};     // nil for v1
  std::string stage = "created";
  std::string created_by_json;  // ProducerInfo
  std::int64_t created_at_ns = 0;
  std::string origin_frame;
  std::string crs;
  std::string status = "open";  // open | read_only | archived
  std::string properties_json;
};

struct SceneVersion {
  Uuid version_id{};            // immutable (ADR-033)
  Uuid scene_id{};
  Uuid parent_version_id{};     // nil for v1
  std::string stage;            // created | imported | aligned | ...
  std::string created_by_json;  // ProducerInfo
  std::int64_t created_at_ns = 0;
  std::string status = "active";
};

}  // namespace spatial::core
