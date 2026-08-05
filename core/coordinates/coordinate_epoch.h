#pragma once

// Geodetic datum/epoch definition (RFC-0002 §7.3). All coordinates expressed
// in a frame carrying this epoch are referenced to datum_id; to_wgs84 maps
// epoch-frame points into the WGS84 (ITRF-class) reference so cross-epoch
// transforms stay well-defined.

#include <limits>
#include <string>

#include "core/coordinates/coordinate_authority.h"
#include "core/coordinates/distance.h"
#include "core/coordinates/timestamp.h"
#include "core/geometry/se3.h"

namespace spatial::core {

struct CoordinateEpoch {
  std::string datum_id;  // e.g. "ITRF2020@2020.0"
  TimestampNs valid_from = TimestampNs(0);
  TimestampNs valid_to = TimestampNs(std::numeric_limits<int64_t>::max());
  CoordinateAuthority authority;
  DistanceMeters accuracy_meters = DistanceMeters(0.0);
  geometry::SE3 to_wgs84 = geometry::SE3::Identity();
};

}  // namespace spatial::core
