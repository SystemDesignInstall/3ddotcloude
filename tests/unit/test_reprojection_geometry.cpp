// P3-impl-8a: canonical projection + reprojection geometry tests.
//
// Covers the first-class camera model table (§4.9), back-projection and
// distortion round-trips (§4.8), fail-closed model selection (including the
// opencv_fisheye -> pinhole substitution negative test), coordinate-frame and
// anti-double-inversion guards, the deterministic residual evaluator and the
// D5 inlier/outlier rule, the reusable reprojection gate, the shared
// closed-square fixture, and the QualityReport round-trip with the new
// additive reprojection keys (D4).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Geometry>

#include "core/errors/project_error.h"
#include "core/geometry/camera_model.h"
#include "core/geometry/reprojection.h"
#include "core/geometry/se3.h"
#include "core/reconstruction/reconstruction.h"
#include "engine/pipeline/quality/quality_report.h"
#include "tests/unit/fixtures/closed_square_reconstruction.h"

namespace spatial {
namespace {

using spatial::core::ErrorCode;
using spatial::core::ReconCamera;
using spatial::core::ValidationError;
using spatial::core::geometry::CameraFromWorld;
using spatial::core::geometry::CameraIntrinsics;
using spatial::core::geometry::CameraModel;
using spatial::core::geometry::CameraModelKind;
using spatial::core::geometry::EvaluateReprojection;
using spatial::core::geometry::InitializeReprojectionViews;
using spatial::core::geometry::CameraView;
using spatial::core::geometry::PassesReprojectionGate;
using spatial::core::geometry::ReprojectionObservation;
using spatial::core::geometry::ReprojectionResidual;
using spatial::core::geometry::ReprojectionThreshold;
using spatial::core::geometry::SE3;
using spatial::core::geometry::Quaternion;
using spatial::core::geometry::WorldFromCamera;

CameraIntrinsics PinholeIntrinsics() {
  CameraIntrinsics k;
  k.fx = 800.0;
  k.fy = 810.0;
  k.cx = 320.0;
  k.cy = 240.0;
  k.width = 640.0;
  k.height = 480.0;
  return k;
}

constexpr double kPixelTol = 1e-5;    // §4.9 round-trip tolerance (all models)
constexpr double kPinholeTol = 1e-6;  // pinhole is exact to floating point
constexpr double kUnitRayTol = 1e-9;  // ‖dir‖ = 1 within 1e-9 (§4.9)

// ---------------------------------------------------------------------------
// Pinhole projection / back-projection
// ---------------------------------------------------------------------------

TEST(Reprojection8a, PinholeProjectionClosedForm) {
  const CameraModel cam = CameraModel::Pinhole(PinholeIntrinsics());
  // Point at (1, 2, 4) camera frame -> (fx*1/4 + cx, fy*2/4 + cy).
  const Eigen::Vector2d u =
      cam.Project(Eigen::Vector3d(1.0, 2.0, 4.0));
  EXPECT_NEAR(u.x(), 800.0 * 0.25 + 320.0, kPinholeTol);
  EXPECT_NEAR(u.y(), 810.0 * 0.5 + 240.0, kPinholeTol);
}

TEST(Reprojection8a, PinholeBackProjectionClosedForm) {
  const CameraModel cam = CameraModel::Pinhole(PinholeIntrinsics());
  const Eigen::Vector3d ray = cam.Unproject(Eigen::Vector2d(420.0, 260.0));
  // U-distorted normalized = ((420-320)/800, (260-240)/810) = (0.125, ~0.02469).
  EXPECT_NEAR(ray.x() / ray.z(), (420.0 - 320.0) / 800.0, kPinholeTol);
  EXPECT_NEAR(ray.y() / ray.z(), (260.0 - 240.0) / 810.0, kPinholeTol);
  EXPECT_NEAR(ray.norm(), 1.0, kUnitRayTol);
}

TEST(Reprojection8a, PinholeUnitRayNormalized) {
  const CameraModel cam = CameraModel::Pinhole(PinholeIntrinsics());
  const Eigen::Vector3d ray = cam.Unproject(Eigen::Vector2d(100.0, 400.0));
  EXPECT_NEAR(ray.norm(), 1.0, kUnitRayTol);
}

// ---------------------------------------------------------------------------
// Round-trip invariants (§4.8/§4.9) for every first-class model
// ---------------------------------------------------------------------------

// 3D -> project -> 2D -> unproject -> unit ray -> recover 3D -> project -> 2D.
// proj(unproj(u)) == u within the model tolerance; ‖dir‖ == 1 within 1e-9.
void CheckPixelRoundTrip(const CameraModel& cam) {
  const double fx = cam.intrinsics().fx;
  const double tol = cam.kind() == CameraModelKind::kPinhole ? kPinholeTol
                                                             : kPixelTol;
  const std::vector<Eigen::Vector2d> pixels = {
      Eigen::Vector2d(cam.intrinsics().cx, cam.intrinsics().cy),
      Eigen::Vector2d(cam.intrinsics().cx + 0.35 * fx,
                      cam.intrinsics().cy - 0.2 * fx),
      Eigen::Vector2d(cam.intrinsics().cx - 0.4 * fx,
                      cam.intrinsics().cy + 0.25 * fx),
      Eigen::Vector2d(cam.intrinsics().cx + 0.1 * fx,
                      cam.intrinsics().cy - 0.45 * fx)};
  for (const Eigen::Vector2d& u : pixels) {
    const Eigen::Vector3d ray = cam.Unproject(u);
    EXPECT_NEAR(ray.norm(), 1.0, kUnitRayTol);
    const Eigen::Vector2d u_back = cam.Project(ray);
    EXPECT_NEAR((u_back - u).norm(), 0.0, tol) << "pixel round-trip";
    // Recover a 3D point at depth 1 along the ray and re-project.
    const Eigen::Vector2d again = cam.Project(ray * 1.0);
    EXPECT_NEAR((again - u).norm(), 0.0, tol) << "depth-1 re-project";
  }
}

TEST(Reprojection8a, PinholePixelRoundTrip) {
  CheckPixelRoundTrip(CameraModel::Pinhole(PinholeIntrinsics()));
}

TEST(Reprojection8a, OpenCvRadialPixelRoundTrip) {
  const CameraModel cam = CameraModel::OpenCvRadial(
      PinholeIntrinsics(), {0.05, 0.01, 0.001, 0.002});
  CheckPixelRoundTrip(cam);
}

TEST(Reprojection8a, OpenCvRadialWithK3PixelRoundTrip) {
  const CameraModel cam = CameraModel::OpenCvRadial(
      PinholeIntrinsics(), {0.05, 0.01, 0.001, 0.002, 0.0005});
  CheckPixelRoundTrip(cam);
}

TEST(Reprojection8a, OpenCvFisheyePixelRoundTrip) {
  const CameraModel cam = CameraModel::OpenCvFisheye(
      PinholeIntrinsics(), {0.01, 0.001, 0.0001, 0.00001});
  CheckPixelRoundTrip(cam);
}

TEST(Reprojection8a, FovPixelRoundTrip) {
  const CameraModel cam = CameraModel::Fov(PinholeIntrinsics(), {0.4});
  CheckPixelRoundTrip(cam);
}

// ---------------------------------------------------------------------------
// Distortion maps: distort -> undistort round-trip and identity for pinhole
// ---------------------------------------------------------------------------

TEST(Reprojection8a, DistortionUndistortionRoundTrip) {
  const CameraModel radial = CameraModel::OpenCvRadial(
      PinholeIntrinsics(), {0.05, 0.01, 0.001, 0.002});
  const CameraModel fisheye = CameraModel::OpenCvFisheye(
      PinholeIntrinsics(), {0.01, 0.001, 0.0001, 0.00001});
  const CameraModel fov = CameraModel::Fov(PinholeIntrinsics(), {0.4});
  const std::vector<Eigen::Vector2d> samples = {
      Eigen::Vector2d(0.1, -0.2), Eigen::Vector2d(-0.3, 0.15),
      Eigen::Vector2d(0.05, 0.05)};
  for (const CameraModel* cam : {&radial, &fisheye, &fov}) {
    for (const auto& x : samples) {
      const Eigen::Vector2d xd = cam->DistortUnit(x);
      const Eigen::Vector2d back = cam->UndistortUnit(xd);
      EXPECT_NEAR((back - x).norm(), 0.0, 1e-5);
    }
  }
  // Pinhole distortion is the identity.
  const CameraModel pinhole = CameraModel::Pinhole(PinholeIntrinsics());
  const Eigen::Vector2d x(0.3, -0.4);
  EXPECT_NEAR((pinhole.DistortUnit(x) - x).norm(), 0.0, 1e-9);
  EXPECT_NEAR((pinhole.UndistortUnit(x) - x).norm(), 0.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Cheirality / invalid inputs (fail closed)
// ---------------------------------------------------------------------------

TEST(Reprojection8a, ProjectionRequiresPositiveDepth) {
  const CameraModel cam = CameraModel::Pinhole(PinholeIntrinsics());
  EXPECT_THROW(cam.Project(Eigen::Vector3d(1.0, 1.0, 0.0)),
               ValidationError);  // w = 0
  EXPECT_THROW(cam.Project(Eigen::Vector3d(1.0, 1.0, -3.0)),
               ValidationError);  // w < 0 (behind camera)
}

TEST(Reprojection8a, ProjectionRejectsNonFinitePoint) {
  const CameraModel cam = CameraModel::Pinhole(PinholeIntrinsics());
  EXPECT_THROW(cam.Project(Eigen::Vector3d(1.0, std::nan(""), 5.0)),
               ValidationError);
  EXPECT_THROW(cam.Project(Eigen::Vector3d(1.0, 1.0,
                                           std::numeric_limits<double>::infinity())),
               ValidationError);
}

TEST(Reprojection8a, BackProjectionRejectsNonFinitePixel) {
  const CameraModel cam = CameraModel::Pinhole(PinholeIntrinsics());
  EXPECT_THROW(cam.Unproject(Eigen::Vector2d(std::nan(""), 10.0)),
               ValidationError);
  EXPECT_THROW(cam.Unproject(Eigen::Vector2d(10.0, -std::numeric_limits<double>::infinity())),
               ValidationError);
}

TEST(Reprojection8a, DegenerateIntrinsicsFailClosed) {
  CameraIntrinsics bad = PinholeIntrinsics();
  bad.fx = 0.0;
  EXPECT_THROW(CameraModel::Pinhole(bad), spatial::core::CalibrationError);
  bad = PinholeIntrinsics();
  bad.fy = -5.0;
  EXPECT_THROW(CameraModel::Pinhole(bad), spatial::core::CalibrationError);
  bad = PinholeIntrinsics();
  bad.cx = std::nan("");
  EXPECT_THROW(CameraModel::Pinhole(bad), spatial::core::CalibrationError);
}

TEST(Reprojection8a, NonFiniteDistortionCoefficientsFailClosed) {
  EXPECT_THROW(
      CameraModel::OpenCvRadial(PinholeIntrinsics(),
                                {0.05, std::nan(""), 0.001, 0.002}),
      ValidationError);
  EXPECT_THROW(CameraModel::Fov(PinholeIntrinsics(), {std::nan("")}),
               ValidationError);
}

TEST(Reprojection8a, WrongCoefficientCountsFailClosed) {
  EXPECT_THROW(CameraModel::OpenCvRadial(PinholeIntrinsics(), {0.05}),
               ValidationError);
  EXPECT_THROW(
      CameraModel::OpenCvFisheye(PinholeIntrinsics(), {0.01, 0.001}),
      ValidationError);
  EXPECT_THROW(CameraModel::Fov(PinholeIntrinsics(), {0.4, 0.1}),
               ValidationError);
}

// ---------------------------------------------------------------------------
// Fail-closed model selection (§4.9 answer B) — never a silent substitution
// ---------------------------------------------------------------------------

ReconCamera MakeReconCamera(const std::string& intrinsic_model,
                            const std::string& distortion_model,
                            const std::vector<double>& coefficients) {
  ReconCamera c;
  c.camera_id = 1;
  c.width = 640;
  c.height = 480;
  c.intrinsic_model = intrinsic_model;
  c.fx = 800.0;
  c.fy = 800.0;
  c.cx = 320.0;
  c.cy = 240.0;
  c.distortion_model = distortion_model;
  c.distortion_coefficients = coefficients;
  return c;
}

TEST(Reprojection8a, FirstClassModelsSelect) {
  const CameraModel pinhole = CameraModel::FromReconCamera(
      MakeReconCamera("pinhole", "none", {}));
  EXPECT_EQ(pinhole.kind(), CameraModelKind::kPinhole);

  const CameraModel opencv = CameraModel::FromReconCamera(
      MakeReconCamera("opencv", "opencv_radial", {0.05, 0.01, 0.001, 0.002}));
  EXPECT_EQ(opencv.kind(), CameraModelKind::kOpenCvRadial);

  const CameraModel fisheye = CameraModel::FromReconCamera(
      MakeReconCamera("opencv_fisheye", "opencv_fisheye",
                      {0.01, 0.001, 0.0001, 0.00001}));
  EXPECT_EQ(fisheye.kind(), CameraModelKind::kOpenCvFisheye);

  const CameraModel fov =
      CameraModel::FromReconCamera(MakeReconCamera("fov", "none", {0.4}));
  EXPECT_EQ(fov.kind(), CameraModelKind::kFov);
}

TEST(Reprojection8a, UnsupportedModelsFailClosed) {
  // Every model outside the first-class set is rejected at model-selection.
  EXPECT_THROW(CameraModel::FromReconCamera(MakeReconCamera(
                   "omnidirectional", "omnidirectional", {})),
               ValidationError);
  EXPECT_THROW(CameraModel::FromReconCamera(MakeReconCamera(
                   "custom", "custom", {0.05, 0.01})),
               ValidationError);
  EXPECT_THROW(CameraModel::FromReconCamera(MakeReconCamera(
                   "opengl", "none", {})),
               ValidationError);
  EXPECT_THROW(CameraModel::FromReconCamera(MakeReconCamera(
                   "", "none", {})),
               ValidationError);
  // First-class intrinsic combined with a non-matching distortion model is
  // also rejected (never approximated).
  EXPECT_THROW(CameraModel::FromReconCamera(MakeReconCamera(
                   "pinhole", "opencv_radial", {0.05})),
               ValidationError);
  EXPECT_THROW(CameraModel::FromReconCamera(MakeReconCamera(
                   "opencv", "none", {})),
               ValidationError);
}

// Negative test: opencv_fisheye -> pinhole substitution is impossible.
// A fisheye camera is first-class (§4.9) and selects kOpenCvFisheye -- it is
// never silently approximated as pinhole. Forcing a fisheye distortion onto a
// pinhole intrinsic fails closed at selection time.
TEST(Reprojection8a, FisheyeNeverSubstitutedAsPinhole) {
  const ReconCamera fisheye = MakeReconCamera(
      "opencv_fisheye", "opencv_fisheye", {0.01, 0.001, 0.0001, 0.00001});
  const CameraModel model = CameraModel::FromReconCamera(fisheye);
  EXPECT_EQ(model.kind(), CameraModelKind::kOpenCvFisheye);
  EXPECT_NE(model.kind(), CameraModelKind::kPinhole);

  // A fisheye projection is materially different from a pinhole projection for
  // the same ray, so a substitution would be caught by the geometry.
  const CameraModel pinhole = CameraModel::Pinhole(PinholeIntrinsics());
  const CameraModel fisheye_strong = CameraModel::OpenCvFisheye(
      PinholeIntrinsics(), {0.5, 0.02, 0.001, 0.0001});
  const Eigen::Vector3d p(1.0, 0.8, 3.0);
  EXPECT_GT((fisheye_strong.Project(p) - pinhole.Project(p)).norm(), 1.0);

  // Substitution via data is rejected at selection time: a fisheye distortion
  // declared on a pinhole intrinsic is not "opencv_fisheye" and fails closed.
  const ReconCamera declared_pinhole = MakeReconCamera(
      "pinhole", "opencv_fisheye", {0.01, 0.001, 0.0001, 0.00001});
  EXPECT_THROW(CameraModel::FromReconCamera(declared_pinhole), ValidationError);
}

// ---------------------------------------------------------------------------
// Coordinate-frame convention (§4.8): T_rc vs T_cr and anti-double-inversion
// ---------------------------------------------------------------------------

TEST(Reprojection8a, InverseRoundTripAndAntiDoubleInversion) {
  const SE3 T_rc(Quaternion::FromAxisAngle(Eigen::Vector3d(0, 0, 1), 0.7),
                 Eigen::Vector3d(1.0, 2.0, 3.0));
  const CameraFromWorld T_cr = WorldFromCamera(T_rc).Inverse();
  const Eigen::Vector3d p_R(0.5, -0.5, 1.0);
  const Eigen::Vector3d p_C = T_cr.TransformPoint(p_R);
  // Anti-double-inversion: applying T_cr to T_rc's output is identity.
  const Eigen::Vector3d back = T_rc.TransformPoint(T_cr.TransformPoint(p_R));
  EXPECT_TRUE((back - p_R).norm() < 1e-9);
  // T_rc . T_cr == Identity.
  const Eigen::Vector3d id = T_rc.TransformPoint(T_cr.TransformPoint(p_C));
  EXPECT_TRUE((id - p_C).norm() < 1e-9);
}

// Negative test: T_rc misused as T_cr (world-from-camera posed as camera-from-
// world) yields a pixel far outside the round-trip tolerance.
TEST(Reprojection8a, T_rcMisusedAsT_crCaughtByTolerance) {
  const CameraModel cam = CameraModel::Pinhole(PinholeIntrinsics());
  const SE3 T_rc(Quaternion::FromAxisAngle(Eigen::Vector3d(0, 1, 0), 0.4),
                 Eigen::Vector3d(4.0, 0.0, 0.0));
  const Eigen::Vector3d p_R(0.0, 0.0, 5.0);
  // Correct: T_cr = T_rc^{-1}.
  const CameraFromWorld correct = WorldFromCamera(T_rc).Inverse();
  const Eigen::Vector2d u_correct =
      cam.Project(correct.TransformPoint(p_R));
  // Wrong: apply T_rc directly as if it were camera-from-world.
  const Eigen::Vector2d u_wrong = cam.Project(T_rc.TransformPoint(p_R));
  EXPECT_GT((u_correct - u_wrong).norm(), 10.0)
      << "using T_rc in place of T_cr must be caught by the geometry";
}

// ---------------------------------------------------------------------------
// Reconstruction feed: InitializeReprojectionViews
// ---------------------------------------------------------------------------

TEST(Reprojection8a, InitializeReprojectionViewsFromReconstruction) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  ASSERT_TRUE(scene.SelfCheck());

  const std::vector<CameraView> views =
      InitializeReprojectionViews(scene.rec);
  ASSERT_EQ(views.size(), 5u);
  // Ordered by image_id (1..5).
  for (std::size_t i = 0; i < views.size(); ++i) {
    EXPECT_EQ(views[i].image_id, static_cast<std::uint32_t>(i + 1));
    EXPECT_EQ(views[i].model.kind(), CameraModelKind::kPinhole);
  }
}

TEST(Reprojection8a, InitializeReprojectionViewsFailsClosedOnUnsupportedModel) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  spatial::core::Reconstruction rec = scene.rec;
  rec.cameras[0].intrinsic_model = "omnidirectional";
  rec.cameras[0].distortion_model = "";
  EXPECT_THROW(InitializeReprojectionViews(rec), ValidationError);
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

TEST(Reprojection8a, FixtureSelfCheck) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  ASSERT_TRUE(scene.SelfCheck());
  ASSERT_EQ(scene.truth_poses.size(), 5u);
  ASSERT_EQ(scene.drifted_poses.size(), 5u);
  ASSERT_EQ(scene.keypoints.size(), 5u);
  ASSERT_EQ(scene.rec.images.size(), 5u);
  ASSERT_EQ(scene.rec.cameras.size(), 1u);
  ASSERT_EQ(scene.rec.points3D.size(), 4u);
  // Each of the 5 frames sees all 4 corners.
  for (const auto& frame_kpts : scene.keypoints) {
    ASSERT_EQ(frame_kpts.size(), 4u);
  }
}

TEST(Reprojection8a, FixtureFailsClosedOnInvalidConstruction) {
  // Non-finite intrinsics -> construction throws.
  CameraIntrinsics bad = spatial::test::BuildDefaultClosedSquareScene().intrinsics;
  bad.fx = std::nan("");
  EXPECT_THROW(spatial::test::BuildClosedSquareScene(bad, 0.1, 0.05),
               ValidationError);
  // Negative depth geometry would put corners behind the camera.
  CameraIntrinsics good = spatial::test::BuildDefaultClosedSquareScene().intrinsics;
  // A pose set that is out of count fails via MakeReconstructionWithPoses.
  const auto scene = spatial::test::BuildDefaultClosedSquareScene();
  std::vector<spatial::core::ReconPose> too_few(scene.truth_poses.begin(),
                                                scene.truth_poses.end() - 1);
  EXPECT_THROW(spatial::test::MakeReconstructionWithPoses(scene, too_few),
               ValidationError);
}

// ---------------------------------------------------------------------------
// Residual evaluator: drift vs truth, determinism, outlier rule (D5)
// ---------------------------------------------------------------------------

std::vector<ReprojectionObservation> ObservationsFromScene(
    const spatial::test::ClosedSquareScene& scene) {
  std::vector<ReprojectionObservation> obs;
  obs.reserve(5u * 4u);
  for (std::size_t i = 0; i < scene.rec.images.size(); ++i) {
    for (std::size_t c = 0; c < scene.rec.points3D.size(); ++c) {
      ReprojectionObservation o;
      o.image_id = scene.rec.images[i].image_id;
      o.point3d_id = scene.rec.points3D[c].point3d_id;
      o.keypoint_2d = Eigen::Vector2d(scene.keypoints[i][c].x,
                                      scene.keypoints[i][c].y);
      obs.push_back(o);
    }
  }
  return obs;
}

std::vector<std::pair<std::uint64_t, std::array<double, 3>>> PointsFromScene(
    const spatial::test::ClosedSquareScene& scene) {
  std::vector<std::pair<std::uint64_t, std::array<double, 3>>> points;
  for (const auto& p : scene.rec.points3D) {
    points.emplace_back(p.point3d_id, p.xyz);
  }
  return points;
}

TEST(Reprojection8a, TruthPosesGiveNearZeroResiduals) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  const auto views = InitializeReprojectionViews(
      spatial::test::MakeReconstructionWithPoses(scene, scene.truth_poses));
  const auto metrics =
      EvaluateReprojection(views, PointsFromScene(scene),
                           ObservationsFromScene(scene));
  EXPECT_EQ(metrics.total_count, 20);
  EXPECT_EQ(metrics.inlier_count, 20);
  EXPECT_EQ(metrics.outlier_count, 0);
  EXPECT_LT(metrics.rmse_px, 1e-5);
  EXPECT_LT(metrics.mean_error_px, 1e-5);
  EXPECT_GE(metrics.threshold_px, 2.0);  // D5 2px floor
  EXPECT_EQ(metrics.per_image.size(), 5u);
  EXPECT_EQ(metrics.per_point.size(), 4u);
}

TEST(Reprojection8a, DriftedPosesProduceLargerResidualsThanTruth) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  const auto drifted_views = InitializeReprojectionViews(scene.rec);
  const auto truth_views = InitializeReprojectionViews(
      spatial::test::MakeReconstructionWithPoses(scene, scene.truth_poses));
  const auto obs = ObservationsFromScene(scene);
  const auto points = PointsFromScene(scene);
  const auto drifted = EvaluateReprojection(drifted_views, points, obs);
  const auto truth = EvaluateReprojection(truth_views, points, obs);
  EXPECT_GT(drifted.rmse_px, truth.rmse_px);
  EXPECT_GT(drifted.mean_error_px, truth.mean_error_px);
  // Per-image aggregates present and deterministic.
  EXPECT_EQ(drifted.per_image.size(), truth.per_image.size());
}

TEST(Reprojection8a, DeterministicMetrics) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  const auto views = InitializeReprojectionViews(scene.rec);
  const auto obs = ObservationsFromScene(scene);
  const auto points = PointsFromScene(scene);
  const auto a = EvaluateReprojection(views, points, obs);
  const auto b = EvaluateReprojection(views, points, obs);
  EXPECT_EQ(a.rmse_px, b.rmse_px);
  EXPECT_EQ(a.mean_error_px, b.mean_error_px);
  EXPECT_EQ(a.median_error_px, b.median_error_px);
  EXPECT_EQ(a.threshold_px, b.threshold_px);
  EXPECT_EQ(a.inlier_count, b.inlier_count);
  EXPECT_EQ(a.outlier_count, b.outlier_count);
  EXPECT_EQ(a.per_image.size(), b.per_image.size());
  EXPECT_EQ(a.per_point.size(), b.per_point.size());
}

TEST(Reprojection8a, OutlierRuleMatchesHandComputedSplit) {
  // Residuals [0.1, 0.2, 0.3, 0.4, 5.0]: median 0.3 -> threshold
  // max(3*0.3, 2.0) = 2.0 -> the 5.0 sample is the ONLY outlier.
  const CameraIntrinsics k = PinholeIntrinsics();
  const CameraModel cam = CameraModel::Pinhole(k);
  const SE3 pose = SE3::Identity();
  std::vector<CameraView> views{CameraView{1, cam, pose}};
  const std::array<double, 3> xyz{0.0, 0.0, 1.0};
  const std::vector<std::pair<std::uint64_t, std::array<double, 3>>> points = {
      {1, xyz}};
  // Build keypoints that yield the crafted residual set: project the point
  // through the view and shift by the desired residual along x.
  const CameraFromWorld cr = WorldFromCamera(pose).Inverse();
  const Eigen::Vector2d projected = cam.Project(cr.TransformPoint(
      Eigen::Vector3d(xyz[0], xyz[1], xyz[2])));
  std::vector<ReprojectionObservation> obs;
  const std::vector<double> residuals = {0.1, 0.2, 0.3, 0.4, 5.0};
  for (const double r : residuals) {
    ReprojectionObservation o;
    o.image_id = 1;
    o.point3d_id = 1;
    o.keypoint_2d = projected + Eigen::Vector2d(r, 0.0);
    obs.push_back(o);
  }
  std::vector<ReprojectionResidual> per_obs;
  const auto metrics =
      EvaluateReprojection(views, points, obs, &per_obs);
  EXPECT_EQ(metrics.total_count, 5);
  EXPECT_NEAR(metrics.median_error_px, 0.3, 1e-6);
  EXPECT_DOUBLE_EQ(metrics.threshold_px, 2.0);
  EXPECT_EQ(metrics.inlier_count, 4);
  EXPECT_EQ(metrics.outlier_count, 1);
  // RMS over the 4 inliers: sqrt((0.1^2+0.2^2+0.3^2+0.4^2)/4).
  const double expected_rms =
      std::sqrt((0.01 + 0.04 + 0.09 + 0.16) / 4.0);
  EXPECT_NEAR(metrics.rmse_px, expected_rms, 1e-12);
  EXPECT_EQ(per_obs.size(), 5u);
  EXPECT_FALSE(per_obs[4].inlier);
  EXPECT_TRUE(per_obs[0].inlier);
}

TEST(Reprojection8a, ReprojectionThresholdD5) {
  EXPECT_DOUBLE_EQ(ReprojectionThreshold(0.1), 2.0);   // 3*0.1=0.3 < 2 floor
  EXPECT_DOUBLE_EQ(ReprojectionThreshold(1.0), 3.0);   // 3*1.0=3.0 > 2
  EXPECT_DOUBLE_EQ(ReprojectionThreshold(0.0), 2.0);   // 2 px floor
}

TEST(Reprojection8a, EvaluatorFailsClosedOnUnknownReferences) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  const auto views = InitializeReprojectionViews(scene.rec);
  const auto obs = ObservationsFromScene(scene);
  const auto points = PointsFromScene(scene);

  std::vector<ReprojectionObservation> bad_image = obs;
  bad_image[0].image_id = 999;
  EXPECT_THROW(EvaluateReprojection(views, points, bad_image),
               ValidationError);

  std::vector<ReprojectionObservation> bad_point = obs;
  bad_point[0].point3d_id = 99999;
  EXPECT_THROW(EvaluateReprojection(views, points, bad_point),
               ValidationError);
}

TEST(Reprojection8a, EvaluatorFailsClosedOnNonFiniteKeypoint) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  const auto views = InitializeReprojectionViews(scene.rec);
  const auto obs = ObservationsFromScene(scene);
  const auto points = PointsFromScene(scene);
  std::vector<ReprojectionObservation> bad = obs;
  bad[0].keypoint_2d = Eigen::Vector2d(std::nan(""), 10.0);
  EXPECT_THROW(EvaluateReprojection(views, points, bad), ValidationError);
}

// ---------------------------------------------------------------------------
// Quality gate predicate (D5)
// ---------------------------------------------------------------------------

TEST(Reprojection8a, GateAcceptsStrictImprovement) {
  EXPECT_TRUE(PassesReprojectionGate(0.8, 1.0));   // 20% better -> pass
  EXPECT_TRUE(PassesReprojectionGate(0.89, 1.0));  // 11% better -> pass
  EXPECT_FALSE(PassesReprojectionGate(0.9, 1.0));  // exactly 10%: strict <
  EXPECT_FALSE(PassesReprojectionGate(0.95, 1.0)); // only 5% -> fail
  EXPECT_FALSE(PassesReprojectionGate(1.2, 1.0));  // worse -> fail
  EXPECT_FALSE(PassesReprojectionGate(std::nan(""), 1.0));
  EXPECT_FALSE(PassesReprojectionGate(0.5, std::nan("")));
  EXPECT_FALSE(PassesReprojectionGate(-1.0, 1.0));
  EXPECT_FALSE(PassesReprojectionGate(0.5, -1.0));
  // Custom factor.
  EXPECT_TRUE(PassesReprojectionGate(0.5, 1.0, 0.9));
  EXPECT_FALSE(PassesReprojectionGate(0.5, 1.0, 0.4));  // 50% margin required
}

TEST(Reprojection8a, GateApplicationGivesNonZeroImprovementForFullChain) {
  // End-to-end shape: v1 (drifted) RMS vs corrected (truth) RMS -> gate passes.
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  const auto obs = ObservationsFromScene(scene);
  const auto points = PointsFromScene(scene);
  const auto drifted_views = InitializeReprojectionViews(scene.rec);
  const auto truth_views = InitializeReprojectionViews(
      spatial::test::MakeReconstructionWithPoses(scene, scene.truth_poses));
  const auto before = EvaluateReprojection(drifted_views, points, obs);
  const auto after = EvaluateReprojection(truth_views, points, obs);
  EXPECT_TRUE(PassesReprojectionGate(after.rmse_px, before.rmse_px, 0.9));
}

// ---------------------------------------------------------------------------
// QualityReport round-trip with the new additive reprojection keys (D4)
// ---------------------------------------------------------------------------

TEST(Reprojection8a, QualityReportRoundTripWithAdditiveKeys) {
  const spatial::test::ClosedSquareScene scene =
      spatial::test::BuildDefaultClosedSquareScene();
  const auto views = InitializeReprojectionViews(scene.rec);
  const auto metrics = EvaluateReprojection(
      views, PointsFromScene(scene), ObservationsFromScene(scene));

  spatial::engine::QualityReport report =
      spatial::engine::EvaluateQuality("hash-8a", "validate",
                                       R"({"config":{}})", {"ref-1"});
  report.reprojection = metrics;
  report.reprojection.rmse_px = 0.5;
  report.reprojection.mean_error_px = 0.4;
  report.reprojection.median_error_px = 0.35;
  report.reprojection.threshold_px = 2.0;
  report.reprojection.inlier_count = 19;
  report.reprojection.outlier_count = 1;
  report.reprojection.total_count = 20;

  const std::string json = spatial::engine::QualityReportToJson(report);
  const auto parsed = spatial::engine::QualityReportFromJson(json);

  EXPECT_EQ(parsed.reprojection.rmse_px,
            report.reprojection.rmse_px);
  EXPECT_EQ(parsed.reprojection.mean_error_px,
            report.reprojection.mean_error_px);
  EXPECT_EQ(parsed.reprojection.median_error_px,
            report.reprojection.median_error_px);
  EXPECT_EQ(parsed.reprojection.threshold_px,
            report.reprojection.threshold_px);
  EXPECT_EQ(parsed.reprojection.inlier_count,
            report.reprojection.inlier_count);
  EXPECT_EQ(parsed.reprojection.outlier_count,
            report.reprojection.outlier_count);
  EXPECT_EQ(parsed.reprojection.total_count,
            report.reprojection.total_count);
  EXPECT_EQ(parsed.reprojection.per_image.size(),
            report.reprojection.per_image.size());
  EXPECT_EQ(parsed.reprojection.per_point.size(),
            report.reprojection.per_point.size());
  for (std::size_t i = 0; i < parsed.reprojection.per_image.size(); ++i) {
    EXPECT_EQ(parsed.reprojection.per_image[i].image_id,
              report.reprojection.per_image[i].image_id);
    EXPECT_EQ(parsed.reprojection.per_image[i].rmse_px,
              report.reprojection.per_image[i].rmse_px);
    EXPECT_EQ(parsed.reprojection.per_image[i].inlier_count,
              report.reprojection.per_image[i].inlier_count);
  }
  for (std::size_t i = 0; i < parsed.reprojection.per_point.size(); ++i) {
    EXPECT_EQ(parsed.reprojection.per_point[i].point3d_id,
              report.reprojection.per_point[i].point3d_id);
    EXPECT_EQ(parsed.reprojection.per_point[i].error_px,
              report.reprojection.per_point[i].error_px);
    EXPECT_EQ(parsed.reprojection.per_point[i].inlier_count,
              report.reprojection.per_point[i].inlier_count);
  }
}

}  // namespace
}  // namespace spatial