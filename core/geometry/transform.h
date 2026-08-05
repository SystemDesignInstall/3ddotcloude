#pragma once

// Rigid transform with explicit directional semantics (RFC-0002 §7.2,
// ADR-018). The field names at the use site (e.g.
// CoordinateFrame::parent_from_child) state which frame maps into which;
// this type is the graph-edge carrier. Public APIs must prefer the named
// strong types in camera_transform.h / rig_transform.h over this type.

#include <Eigen/Geometry>

#include "core/geometry/se3.h"

namespace spatial::core::geometry {

class RigidTransform {
 public:
  RigidTransform() = default;
  explicit RigidTransform(SE3 value) : value_(value) {}

  static RigidTransform Identity() { return RigidTransform(SE3::Identity()); }

  const SE3& AsSe3() const noexcept { return value_; }

  RigidTransform Inverse() const { return RigidTransform(value_.Inverse()); }

  Eigen::Vector3d TransformPoint(const Eigen::Vector3d& p) const {
    return value_.TransformPoint(p);
  }

  friend RigidTransform operator*(const RigidTransform& a,
                                  const RigidTransform& b) {
    return RigidTransform(a.value_ * b.value_);
  }

 private:
  SE3 value_;
};

}  // namespace spatial::core::geometry
