#pragma once

// SE(3) rigid body transform (RFC-0002 §7.2). p' = R * p + t, active
// right-handed convention (docs/architecture/coordinate-systems.md).
// Rotation is stored as a Quaternion; the platform stores no Euler angles.

#include <Eigen/Geometry>

#include "core/geometry/quaternion.h"

namespace spatial::core::geometry {

class SE3 {
 public:
  SE3() : rotation_(Quaternion::Identity()),
          translation_(Eigen::Vector3d::Zero()) {}
  SE3(Quaternion rotation, Eigen::Vector3d translation)
      : rotation_(rotation), translation_(translation) {}

  static SE3 Identity() {
    return SE3(Quaternion::Identity(), Eigen::Vector3d::Zero());
  }

  const Quaternion& rotation() const noexcept { return rotation_; }
  const Eigen::Vector3d& translation() const noexcept { return translation_; }

  // Homogeneous form. For adapter boundaries only; domain code composes SE3.
  Eigen::Matrix4d ToMatrix() const {
    Eigen::Matrix4d m = Eigen::Matrix4d::Identity();
    m.block<3, 3>(0, 0) = rotation_.ToRotationMatrix();
    m.block<3, 1>(0, 3) = translation_;
    return m;
  }

  static SE3 FromMatrix(const Eigen::Matrix4d& m) {
    return SE3(Quaternion::FromRotationMatrix(m.block<3, 3>(0, 0)),
               m.block<3, 1>(0, 3));
  }

  SE3 Inverse() const {
    const Eigen::Vector3d t_inv =
        -rotation_.Inverse().Rotate(translation_);
    return SE3(rotation_.Inverse(), t_inv);
  }

  Eigen::Vector3d TransformPoint(const Eigen::Vector3d& p) const {
    return rotation_.Rotate(p) + translation_;
  }

  // Composition: (a * b)(p) == a(b(p)).
  friend SE3 operator*(const SE3& a, const SE3& b) {
    return SE3(a.rotation_ * b.rotation_,
               a.rotation_.Rotate(b.translation_) + a.translation_);
  }

 private:
  Quaternion rotation_;
  Eigen::Vector3d translation_;
};

}  // namespace spatial::core::geometry
