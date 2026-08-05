#pragma once

// Typed extrinsic transforms between the world and a camera (RFC-0002 §7.2).
// The type names encode the mapping direction so it cannot be swapped at a
// call site (ADR-018).

#include <Eigen/Geometry>

#include "core/geometry/se3.h"

namespace spatial::core::geometry {

class CameraFromWorld;

class WorldFromCamera {
 public:
  explicit WorldFromCamera(SE3 value) : value_(value) {}

  const SE3& AsSe3() const noexcept { return value_; }

  Eigen::Vector3d TransformPoint(const Eigen::Vector3d& p) const {
    return value_.TransformPoint(p);
  }

  CameraFromWorld Inverse() const;

 private:
  SE3 value_;
};

class CameraFromWorld {
 public:
  explicit CameraFromWorld(SE3 value) : value_(value) {}

  const SE3& AsSe3() const noexcept { return value_; }

  Eigen::Vector3d TransformPoint(const Eigen::Vector3d& p) const {
    return value_.TransformPoint(p);
  }

  WorldFromCamera Inverse() const;

 private:
  SE3 value_;
};

inline CameraFromWorld WorldFromCamera::Inverse() const {
  return CameraFromWorld(value_.Inverse());
}

inline WorldFromCamera CameraFromWorld::Inverse() const {
  return WorldFromCamera(value_.Inverse());
}

}  // namespace spatial::core::geometry
