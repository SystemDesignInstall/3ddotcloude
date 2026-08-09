#pragma once

// Frame — the kinematic frame for one exposure (scene-model.md §4.2,
// image-import.md §4). Immutable once written. At import time pose_ref is nil
// (no pose is estimated by the importer; PPS-0001 §5.9).

#include <cstdint>
#include <string>

#include "core/coordinates/timestamp.h"
#include "core/utils/uuid.h"

namespace spatial::core {

struct Frame {
  Uuid frame_id{};            // UUIDv5, deterministic (core/scene/identity.h)
  Uuid scene_id{};
  Uuid session_id{};          // mandatory (RFC-0002, PPS-0001 §5.2)
  TimestampNs timestamp_ns{0};
  std::int64_t sequence_index = 0;
  Uuid sensor_id{};
  Uuid pose_ref{};            // nil at import; resolved by downstream stages
  std::string properties_json;
};

}  // namespace spatial::core
