#pragma once

// Quaternion in scalar-last storage [x, y, z, w] (ADR-007/018,
// docs/architecture/coordinate-systems.md). Rotation convention is the
// platform's active, right-handed frame: applying q to a point p gives
// q * p = q.rotate(p). No Euler angles exist anywhere in the platform.

#include <cmath>

#include <Eigen/Geometry>

namespace spatial::core::geometry {

class Quaternion {
 public:
  Quaternion() : q_(0.0, 0.0, 0.0, 1.0) {}  // identity
  explicit Quaternion(double x, double y, double z, double w)
      : q_(w, x, y, z) {}

  static Quaternion Identity() { return Quaternion(0.0, 0.0, 0.0, 1.0); }

  static Quaternion FromRotationMatrix(const Eigen::Matrix3d& m) {
    return Quaternion(Eigen::Quaterniond(m));
  }

  static Quaternion FromAxisAngle(const Eigen::Vector3d& axis, double angle) {
    return Quaternion(Eigen::Quaterniond(Eigen::AngleAxisd(angle, axis)));
  }

  double x() const noexcept { return q_.x(); }
  double y() const noexcept { return q_.y(); }
  double z() const noexcept { return q_.z(); }
  double w() const noexcept { return q_.w(); }

  // Storage order [x, y, z, w] matches Eigen::Quaterniond::coeffs().
  Eigen::Vector4d Coefficients() const { return q_.coeffs(); }

  Eigen::Matrix3d ToRotationMatrix() const { return q_.toRotationMatrix(); }

  Quaternion Normalized() const { return Quaternion(q_.normalized()); }

  Quaternion Inverse() const { return Quaternion(q_.inverse()); }

  Eigen::Vector3d Rotate(const Eigen::Vector3d& p) const { return q_ * p; }

  double Norm() const { return q_.norm(); }

  friend Quaternion operator*(const Quaternion& a, const Quaternion& b) {
    return Quaternion(a.q_ * b.q_);
  }

 private:
  explicit Quaternion(const Eigen::Quaterniond& q) : q_(q) {}

  Eigen::Quaterniond q_;
};

}  // namespace spatial::core::geometry
