#pragma once

// P3.1 Step 8: engine-facing construction helper for the FROZEN 8b
// re-triangulation observation type (core/geometry/triangulation.h).
//
// TriangulationObservation carries an Eigen member (keypoint_2d). engine/ is
// Eigen-free by convention (check_domain_types.py keeps raw linear algebra
// inside core/geometry/ and adapters/), so the host runner builds observations
// through this thin canonical helper instead of naming a raw Eigen type.
//
// Additive only: triangulation.h and the frozen Retriangulate contract are
// untouched; this header adds no geometry, no policy, and no state.

#include <cstdint>

#include "core/geometry/triangulation.h"

namespace spatial::core::geometry {

// Builds one resolved 2D observation from plain scalars: the pixel (x, y) of
// keypoints[point2d_idx] in the FeatureArtifact of `image_id`.
inline TriangulationObservation MakeTriangulationObservation(
    std::uint32_t image_id, std::int32_t point2d_idx, double x, double y) {
  TriangulationObservation obs;
  obs.image_id = image_id;
  obs.point2d_idx = point2d_idx;
  obs.keypoint_2d = Eigen::Vector2d(x, y);
  return obs;
}

}  // namespace spatial::core::geometry
