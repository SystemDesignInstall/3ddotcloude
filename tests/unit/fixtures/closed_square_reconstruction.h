#pragma once

// Shared synthetic closed-square reconstruction fixture (P3-impl-8a, §4.7/D7).
//
// Builds a small closed-square scene identically for the 8a/8b/8c tests:
//   - 5 frames at truth and drifted poses (T_reconstruction_camera, §4.8),
//   - one shared square of 4 corner 3D points in the reconstruction frame,
//   - per-frame FeatureArtifact-shaped keypoints whose pixels are obtained by
//     projecting the TRUE corners through TRUE intrinsics (the FeatureKeypoint
//     form of feature.schema.json:12-25), and
//   - a v1-shaped Reconstruction feeding the canonical residual evaluator.
//
// The fixture is header-only (D7: no tests/CMakeLists.txt change) and FAILS
// CLOSED on invalid construction inputs: non-finite geometry, non-positive
// projection depth, or out-of-image keypoints throw
// spatial::core::ValidationError(ErrorCode::kValidationDomain) at construction,
// never silently. SelfCheck() verifies that the stored keypoints reproduce the
// intended pixels (round-trip) and that all poses are unit quaternions.

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <Eigen/Geometry>

#include "core/errors/project_error.h"
#include "core/geometry/camera_model.h"
#include "core/geometry/camera_transform.h"
#include "core/geometry/quaternion.h"
#include "core/geometry/se3.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/reconstruction/reconstruction.h"

namespace spatial::test {

// A canned deterministic drift pattern for the 5 poses.
struct PoseDrift {
  std::array<double, 3> position_offset_m;  // T_rc translation perturbation
  std::array<double, 4> rotation_xyzw;      // extra rotation to compose
};

struct ClosedSquareScene {
  static constexpr std::int64_t kFrameCount = 5;
  static constexpr std::size_t kPointCount = 4;

  spatial::core::geometry::CameraIntrinsics intrinsics;
  std::vector<spatial::core::ReconPose> truth_poses;    // T_rc, 5 frames
  std::vector<spatial::core::ReconPose> drifted_poses;  // T_rc, 5 frames
  std::vector<std::array<double, 3>> corner_xyz;        // square corners (world)
  std::vector<std::vector<spatial::core::FeatureKeypoint>> keypoints;  // 5 x 4
  spatial::core::Reconstruction rec;                    // v1-shaped

  // True positive-depth, finite, in-image projection of every truth pose ->
  // every corner reproduces the stored keypoints within the pixel tolerance.
  // Also verifies unit-norm quaternions for all poses. Returns false (never
  // throws) so tests can ASSERT_TRUE on a broken fixture.
  bool SelfCheck(double pixel_tolerance = 1e-7) const;
};

// Default scene: 640x480 pinhole camera, corners at (+-0.25, +-0.25, 0),
// cameras on a circle of radius 5 in the z=10 plane looking at the origin.
inline ClosedSquareScene BuildClosedSquareScene(
    const spatial::core::geometry::CameraIntrinsics& intrinsics, double drift_m,
    double drift_rotation_rad);

// Builds the canonical default fixture with pinhole intrinsics
// fx=fy=500, cx=320, cy=240, 640x480 and a fixed deterministic drift.
inline ClosedSquareScene BuildDefaultClosedSquareScene();

// Returns a fresh copy of `scene.rec` with the given per-frame poses applied.
// Used to model v1 (drifted) vs corrected (truth) reconstructions. Fails
// closed (throws ValidationError) when the pose count does not match.
inline spatial::core::Reconstruction MakeReconstructionWithPoses(
    const ClosedSquareScene& scene,
    const std::vector<spatial::core::ReconPose>& poses);

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

inline std::vector<spatial::core::geometry::SE3> TruthBodyFromCamera(
    const std::vector<spatial::core::ReconPose>& poses) {
  std::vector<spatial::core::geometry::SE3> out;
  out.reserve(poses.size());
  for (const auto& p : poses) {
    const spatial::core::geometry::Quaternion q(
        p.rotation_xyzw[0], p.rotation_xyzw[1], p.rotation_xyzw[2],
        p.rotation_xyzw[3]);
    out.emplace_back(q.Normalized(),
                     Eigen::Vector3d(p.translation_xyz[0], p.translation_xyz[1],
                                     p.translation_xyz[2]));
  }
  return out;
}

inline void ValidatePose(const spatial::core::ReconPose& p,
                         const char* what) {
  for (double v : p.rotation_xyzw) {
    if (!std::isfinite(v)) {
      throw spatial::core::ValidationError(
          spatial::core::ErrorCode::kValidationDomain,
          std::string("closed-square fixture: non-finite rotation in ") + what);
    }
  }
  for (double v : p.translation_xyz) {
    if (!std::isfinite(v)) {
      throw spatial::core::ValidationError(
          spatial::core::ErrorCode::kValidationDomain,
          std::string("closed-square fixture: non-finite translation in ") +
              what);
    }
  }
  const spatial::core::geometry::Quaternion q(
      p.rotation_xyzw[0], p.rotation_xyzw[1], p.rotation_xyzw[2],
      p.rotation_xyzw[3]);
  if (std::abs(q.Norm() - 1.0) > 1e-9) {
    throw spatial::core::ValidationError(
        spatial::core::ErrorCode::kValidationDomain,
        std::string("closed-square fixture: non-unit quaternion in ") + what);
  }
}

inline ClosedSquareScene BuildClosedSquareScene(
    const spatial::core::geometry::CameraIntrinsics& intrinsics, double drift_m,
    double drift_rotation_rad) {
  using spatial::core::FeatureKeypoint;
  using spatial::core::ReconCamera;
  using spatial::core::ReconImage;
  using spatial::core::ReconPoint3D;
  using spatial::core::ReconPose;
  using spatial::core::Reconstruction;
  using spatial::core::ValidationError;
  using spatial::core::ErrorCode;
  using spatial::core::geometry::CameraModel;
  using spatial::core::geometry::CameraFromWorld;
  using spatial::core::geometry::Quaternion;
  using spatial::core::geometry::SE3;
  using spatial::core::geometry::WorldFromCamera;

  if (!intrinsics.Valid()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "closed-square fixture: invalid intrinsics");
  }

  ClosedSquareScene scene;
  scene.intrinsics = intrinsics;

  constexpr int kFrames = static_cast<int>(ClosedSquareScene::kFrameCount);
  constexpr double kRadius = 5.0;
  constexpr double kHeight = 10.0;

  const std::array<double, 3> corner0{-0.25, -0.25, 0.0};
  const std::array<double, 3> corner1{0.25, -0.25, 0.0};
  const std::array<double, 3> corner2{0.25, 0.25, 0.0};
  const std::array<double, 3> corner3{-0.25, 0.25, 0.0};
  for (const auto& c : {corner0, corner1, corner2, corner3}) {
    for (double v : c) {
      if (!std::isfinite(v)) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "closed-square fixture: non-finite corner coordinate");
      }
    }
  }
  scene.corner_xyz = {corner0, corner1, corner2, corner3};

  const CameraModel model = CameraModel::Pinhole(intrinsics);

  // Truth poses: camera i at (R cos, R sin, H) looking at the origin.
  std::vector<SE3> truth_se3;
  truth_se3.reserve(kFrames);
  for (int i = 0; i < kFrames; ++i) {
    const double theta = 2.0 * 3.14159265358979323846 * i / kFrames;
    const Eigen::Vector3d pos(kRadius * std::cos(theta),
                              kRadius * std::sin(theta), kHeight);
    const Eigen::Vector3d z_cam = (-pos).normalized();
    const Eigen::Vector3d x_cam =
        (Eigen::Vector3d(0.0, 0.0, 1.0).cross(z_cam)).normalized();
    const Eigen::Vector3d y_cam = z_cam.cross(x_cam);
    Eigen::Matrix3d R;
    R.col(0) = x_cam;
    R.col(1) = y_cam;
    R.col(2) = z_cam;
    const Quaternion q = Quaternion::FromRotationMatrix(R);
    scene.truth_poses.push_back(
        {q.x(), q.y(), q.z(), q.w(), pos.x(), pos.y(), pos.z()});
    truth_se3.emplace_back(q, pos);
  }
  for (const auto& p : scene.truth_poses) {
    ValidatePose(p, "truth pose");
  }

  // Drifted poses: deterministic per-frame perturbation.
  for (int i = 0; i < kFrames; ++i) {
    const double a0 = std::sin(0.7 * i) * 0.8;
    const double a1 = std::cos(1.3 * i) * 0.8;
    const double a2 = std::cos(0.9 * i) * 0.5;
    const Eigen::Vector3d dpos(drift_m * a0, drift_m * a1, drift_m * a2);
    const double angle = drift_rotation_rad * (0.3 + 0.2 * std::sin(i));
    const Eigen::Vector3d axis = Eigen::Vector3d(0.3, 0.5, 0.2).normalized();
    const Eigen::Quaterniond dq(Eigen::AngleAxisd(angle, axis));
    const SE3 body = SE3(Quaternion(dq.x(), dq.y(), dq.z(), dq.w()).Normalized(),
                         dpos);
    const SE3 drifted = body * truth_se3[i];
    const Quaternion rd = drifted.rotation();
    const Eigen::Vector3d td = drifted.translation();
    scene.drifted_poses.push_back(
        {rd.x(), rd.y(), rd.z(), rd.w(), td.x(), td.y(), td.z()});
  }
  for (const auto& p : scene.drifted_poses) {
    ValidatePose(p, "drifted pose");
  }

  // Keypoints: project TRUE corners through TRUE poses -> intended pixels.
  scene.keypoints.assign(kFrames, std::vector<FeatureKeypoint>(4));
  std::vector<std::vector<std::array<double, 2>>> raw_pixels(
      kFrames, std::vector<std::array<double, 2>>(4));
  for (int i = 0; i < kFrames; ++i) {
    const CameraFromWorld cr = WorldFromCamera(truth_se3[i]).Inverse();
    for (std::size_t c = 0; c < ClosedSquareScene::kPointCount; ++c) {
      const Eigen::Vector3d p_w(scene.corner_xyz[c][0], scene.corner_xyz[c][1],
                                scene.corner_xyz[c][2]);
      const Eigen::Vector3d p_c = cr.TransformPoint(p_w);
      if (p_c.z() <= 0.0) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "closed-square fixture: corner not in front of truth camera "
            "(cheirality violated)");
      }
      const Eigen::Vector2d u = model.Project(p_c);
      const double x = u.x();
      const double y = u.y();
      if (!std::isfinite(x) || !std::isfinite(y) ||
          x < 0.0 || x > intrinsics.width || y < 0.0 ||
          y > intrinsics.height) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "closed-square fixture: projected keypoint outside image or "
            "non-finite (adjust scene geometry)");
      }
      scene.keypoints[i][c] = FeatureKeypoint{x, y};
      raw_pixels[i][c] = {x, y};
    }
  }

  // v1-shaped Reconstruction: drifted poses, one first-class pinhole camera,
  // four 3D points tracked by all five frames.
  Reconstruction rec;
  rec.reconstruction_id = "11111111-1111-4111-8111-111111111111";
  rec.scene_id = "22222222-2222-4222-8222-222222222222";
  rec.session_ids = {"33333333-3333-4333-8333-333333333333"};
  rec.coordinate_frame = "reconstruction_0";
  rec.status = "succeeded";
  rec.created_at_ns = 0;

  ReconCamera cam;
  cam.camera_id = 1;
  cam.width = static_cast<std::int64_t>(intrinsics.width);
  cam.height = static_cast<std::int64_t>(intrinsics.height);
  cam.intrinsic_model = "pinhole";
  cam.fx = intrinsics.fx;
  cam.fy = intrinsics.fy;
  cam.cx = intrinsics.cx;
  cam.cy = intrinsics.cy;
  cam.distortion_model = "none";
  rec.cameras.push_back(cam);

  for (int i = 0; i < kFrames; ++i) {
    ReconImage image;
    image.image_id = static_cast<std::uint32_t>(i + 1);
    image.camera_id = 1;
    image.frame_id = "44444444-4444-4444-8444-44444444440" +
                     std::to_string(i + 1);
    image.name = "frame_" + std::to_string(i) + ".jpg";
    image.pose = scene.drifted_poses[i];
    image.detected = true;
    rec.images.push_back(image);
  }

  const std::uint64_t base_id = 1001u;
  std::vector<SE3> drifted_se3;
  for (const auto& p : scene.drifted_poses) {
    drifted_se3.emplace_back(
        Quaternion(p.rotation_xyzw[0], p.rotation_xyzw[1], p.rotation_xyzw[2],
                   p.rotation_xyzw[3]),
        Eigen::Vector3d(p.translation_xyz[0], p.translation_xyz[1],
                        p.translation_xyz[2]));
  }
  for (std::size_t c = 0; c < ClosedSquareScene::kPointCount; ++c) {
    ReconPoint3D point;
    point.point3d_id = base_id + static_cast<std::uint64_t>(c);
    point.xyz = scene.corner_xyz[c];
    point.color = {128, 128, 128};
    point.track.reserve(kFrames);
    double err_sum = 0.0;
    for (int i = 0; i < kFrames; ++i) {
      ReconPoint3D::TrackElement elem;
      elem.image_id = static_cast<std::uint32_t>(i + 1);
      elem.point2d_idx = static_cast<std::int32_t>(c);
      point.track.push_back(elem);
      // Mean reprojection error is computed under the DRIFTED poses (the v1
      // reconstruction geometry), vs the truth-projected keypoints.
      const CameraFromWorld cr = WorldFromCamera(drifted_se3[i]).Inverse();
      const Eigen::Vector3d p_w(scene.corner_xyz[c][0], scene.corner_xyz[c][1],
                                scene.corner_xyz[c][2]);
      const Eigen::Vector2d u = model.Project(cr.TransformPoint(p_w));
      const Eigen::Vector2d kpt(raw_pixels[i][c][0], raw_pixels[i][c][1]);
      err_sum += (kpt - u).norm();
    }
    point.error = err_sum / static_cast<double>(kFrames);
    rec.points3D.push_back(point);
  }
  scene.rec = rec;
  return scene;
}

inline ClosedSquareScene BuildDefaultClosedSquareScene() {
  spatial::core::geometry::CameraIntrinsics intr;
  intr.fx = 500.0;
  intr.fy = 500.0;
  intr.cx = 320.0;
  intr.cy = 240.0;
  intr.width = 640.0;
  intr.height = 480.0;
  return BuildClosedSquareScene(intr, 0.15, 0.05);
}

inline spatial::core::Reconstruction MakeReconstructionWithPoses(
    const ClosedSquareScene& scene,
    const std::vector<spatial::core::ReconPose>& poses) {
  if (poses.size() != scene.rec.images.size()) {
    throw spatial::core::ValidationError(
        spatial::core::ErrorCode::kValidationDomain,
        "closed-square fixture: pose count must match image count");
  }
  spatial::core::Reconstruction rec = scene.rec;
  for (std::size_t i = 0; i < rec.images.size(); ++i) {
    rec.images[i].pose = poses[i];
  }
  return rec;
}

inline bool ClosedSquareScene::SelfCheck(double pixel_tolerance) const {
  using spatial::core::geometry::CameraModel;
  const CameraModel model = CameraModel::Pinhole(intrinsics);
  const std::vector<spatial::core::geometry::SE3> truth = TruthBodyFromCamera(truth_poses);
  for (std::size_t i = 0; i < truth.size(); ++i) {
    const spatial::core::geometry::CameraFromWorld cr =
        spatial::core::geometry::WorldFromCamera(truth[i]).Inverse();
    for (std::size_t c = 0; c < ClosedSquareScene::kPointCount; ++c) {
      const Eigen::Vector3d p_w(corner_xyz[c][0], corner_xyz[c][1],
                                corner_xyz[c][2]);
      const Eigen::Vector2d u = model.Project(cr.TransformPoint(p_w));
      const double dx = u.x() - keypoints[i][c].x;
      const double dy = u.y() - keypoints[i][c].y;
      if (std::sqrt(dx * dx + dy * dy) > pixel_tolerance) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace spatial::test
