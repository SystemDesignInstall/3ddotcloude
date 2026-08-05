#include <gtest/gtest.h>

#include <cmath>

#include <Eigen/Geometry>

#include "core/geometry/camera_transform.h"
#include "core/geometry/quaternion.h"
#include "core/geometry/rig_transform.h"
#include "core/geometry/se3.h"
#include "core/geometry/transform.h"

namespace spatial::core::geometry {
namespace {

constexpr double kTol = 1e-9;

Eigen::Vector3d V(double x, double y, double z) {
  return Eigen::Vector3d(x, y, z);
}

TEST(Geometry, QuaternionRoundTrip) {
  const Quaternion q = Quaternion::FromAxisAngle(V(0, 0, 1), 0.7);
  const Eigen::Matrix3d m = q.ToRotationMatrix();
  const Quaternion back = Quaternion::FromRotationMatrix(m);
  EXPECT_NEAR((q * back.Inverse()).ToRotationMatrix().trace(), 3.0, kTol);
  EXPECT_NEAR((q * back.Inverse()).Norm(), 1.0, kTol);
}

TEST(Geometry, QuaternionScalarLastStorage) {
  const Quaternion q = Quaternion::FromAxisAngle(V(1, 0, 0), 0.5);
  const Eigen::Vector4d coeffs = q.Coefficients();
  const double w = std::cos(0.5 / 2.0);
  EXPECT_NEAR(coeffs[3], w, kTol);
  EXPECT_NEAR(coeffs.x(), std::sin(0.5 / 2.0), kTol);
}

TEST(Geometry, QuaternionInverseIsReverseRotation) {
  const Quaternion q = Quaternion::FromAxisAngle(V(0, 1, 0), 1.2);
  const Eigen::Vector3d p = V(1.0, 2.0, 3.0);
  const Eigen::Vector3d rotated = q.Rotate(p);
  const Eigen::Vector3d back = q.Inverse().Rotate(rotated);
  EXPECT_TRUE((back - p).norm() < kTol);
}

TEST(Geometry, Se3InverseIsIdentity) {
  const Quaternion r = Quaternion::FromAxisAngle(V(0, 1, 0), 0.9);
  const SE3 t(r, V(1.0, -2.0, 3.0));
  const SE3 product = t.Inverse() * t;
  const Eigen::Vector3d p = V(4.0, 5.0, 6.0);
  EXPECT_TRUE((product.TransformPoint(p) - p).norm() < kTol);
  EXPECT_TRUE((t.Inverse().TransformPoint(t.TransformPoint(p)) - p).norm() < kTol);
}

TEST(Geometry, Se3CompositionMatchesSequentialApplication) {
  const SE3 a(Quaternion::FromAxisAngle(V(1, 0, 0), 0.4), V(1.0, 2.0, 3.0));
  const SE3 b(Quaternion::FromAxisAngle(V(0, 0, 1), -0.6), V(-1.0, 0.5, 2.0));
  const Eigen::Vector3d p = V(0.1, -0.2, 0.3);
  const Eigen::Vector3d sequential = a.TransformPoint(b.TransformPoint(p));
  const Eigen::Vector3d composed = (a * b).TransformPoint(p);
  EXPECT_TRUE((composed - sequential).norm() < kTol);
}

TEST(Geometry, Se3MatrixRoundTrip) {
  const SE3 t(Quaternion::FromAxisAngle(V(0, 0, 1), 1.1), V(0.5, -0.5, 1.0));
  const SE3 back = SE3::FromMatrix(t.ToMatrix());
  const Eigen::Vector3d p = V(1.0, 1.0, 1.0);
  EXPECT_TRUE((back.TransformPoint(p) - t.TransformPoint(p)).norm() < kTol);
}

TEST(Geometry, TypedCameraTransformsRoundTrip) {
  const WorldFromCamera w2c(SE3(Quaternion::FromAxisAngle(V(0, 0, 1), 0.3),
                                V(1.0, 2.0, 3.0)));
  const CameraFromWorld c2w = w2c.Inverse();
  const Eigen::Vector3d p = V(0.0, 0.0, 0.0);
  const Eigen::Vector3d back = c2w.TransformPoint(w2c.TransformPoint(p));
  EXPECT_TRUE((back - p).norm() < kTol);
  static_assert(std::is_same_v<decltype(w2c.Inverse()), CameraFromWorld>);
  static_assert(std::is_same_v<decltype(c2w.Inverse()), WorldFromCamera>);
}

TEST(Geometry, TypedRigTransformsRoundTrip) {
  const RigFromSensor r2s(SE3(Quaternion::Identity(), V(0.1, 0.2, 0.3)));
  const SensorFromRig s2r = r2s.Inverse();
  const Eigen::Vector3d p = V(0.5, -0.5, 0.5);
  const Eigen::Vector3d back = s2r.TransformPoint(r2s.TransformPoint(p));
  EXPECT_TRUE((back - p).norm() < kTol);
}

TEST(Geometry, RigidTransformChain) {
  const RigidTransform world_from_rig(SE3(Quaternion::Identity(), V(1.0, 0.0, 0.0)));
  const RigidTransform rig_from_sensor(SE3(Quaternion::Identity(), V(0.0, 1.0, 0.0)));
  const RigidTransform world_from_sensor = world_from_rig * rig_from_sensor;
  const Eigen::Vector3d p = V(0.0, 0.0, 0.0);
  const Eigen::Vector3d direct = world_from_sensor.TransformPoint(p);
  const Eigen::Vector3d sequential =
      world_from_rig.TransformPoint(rig_from_sensor.TransformPoint(p));
  EXPECT_TRUE((direct - sequential).norm() < kTol);
}

}  // namespace
}  // namespace spatial::core::geometry
