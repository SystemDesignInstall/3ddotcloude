#pragma once

// Minimal domain vector math for pose-graph loop-closure scale resolution
// (P3 D6 / Option-1). These operate on canonical 3-element arrays and rotate
// via the empowered Quaternion type, keeping raw Eigen inside core/geometry/.
//
// Business logic (core/trajectory, engine/) must call these instead of
// spelling Eigen::Vector3d (check_domain_types: only core/geometry/ and
// adapters/ may contain raw Eigen tokens).

#include <array>
#include <cmath>

#include "core/geometry/quaternion.h"

namespace spatial::core::geometry {

// Rotates a 3-vector d by quaternion q (active, right-handed), returning the
// rotated vector in canonical array form. Used for t_W = R_ws * t_ess_unit.
inline std::array<double, 3> RotateDirection(
    const Quaternion& q, const std::array<double, 3>& d) {
  const Eigen::Vector3d v(d[0], d[1], d[2]);
  const Eigen::Vector3d r = q.Rotate(v);
  return {r.x(), r.y(), r.z()};
}

// Dot product of two 3-vectors.
inline double Dot3(const std::array<double, 3>& a,
                   const std::array<double, 3>& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// L2 norm of a 3-vector.
inline double Norm3(const std::array<double, 3>& a) {
  return std::sqrt(Dot3(a, a));
}

}  // namespace spatial::core::geometry
