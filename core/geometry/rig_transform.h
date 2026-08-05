#pragma once

// Typed extrinsic transforms between a sensor rig and an individual sensor
// (RFC-0002 §7.2). The type names encode the mapping direction so it cannot
// be swapped at a call site (ADR-018).

#include <Eigen/Geometry>

#include "core/geometry/se3.h"

namespace spatial::core::geometry {

class SensorFromRig;

class RigFromSensor {
 public:
  explicit RigFromSensor(SE3 value) : value_(value) {}

  const SE3& AsSe3() const noexcept { return value_; }

  Eigen::Vector3d TransformPoint(const Eigen::Vector3d& p) const {
    return value_.TransformPoint(p);
  }

  SensorFromRig Inverse() const;

 private:
  SE3 value_;
};

class SensorFromRig {
 public:
  explicit SensorFromRig(SE3 value) : value_(value) {}

  const SE3& AsSe3() const noexcept { return value_; }

  Eigen::Vector3d TransformPoint(const Eigen::Vector3d& p) const {
    return value_.TransformPoint(p);
  }

  RigFromSensor Inverse() const;

 private:
  SE3 value_;
};

inline SensorFromRig RigFromSensor::Inverse() const {
  return SensorFromRig(value_.Inverse());
}

inline RigFromSensor SensorFromRig::Inverse() const {
  return RigFromSensor(value_.Inverse());
}

}  // namespace spatial::core::geometry
