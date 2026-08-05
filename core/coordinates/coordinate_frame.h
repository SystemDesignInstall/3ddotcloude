#pragma once

// Node in a frame graph (RFC-0002 §7.3). parent_from_child maps child-frame
// points into the parent frame: p_parent = parent_from_child * p_child.
// A nil parent marks the graph root; the graph admits exactly one root.

#include <string>

#include "core/coordinates/coordinate_epoch.h"
#include "core/coordinates/frame_id.h"
#include "core/geometry/transform.h"

namespace spatial::core {

struct CoordinateFrame {
  FrameId id;
  std::string name;  // human-readable label, not unique
  FrameId parent;    // FrameId::Nil() marks the root
  geometry::RigidTransform parent_from_child;
  CoordinateEpoch epoch;
};

}  // namespace spatial::core
