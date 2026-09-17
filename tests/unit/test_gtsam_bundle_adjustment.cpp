// P3-impl-8c: GTSAM Bundle Adjustment adapter DIRECT tests (real LM backend).
//
// GA1 proves REAL refinement through the production seam only: the source
// carries poses with points displaced from the exact rendered observations, so
// the D5 before-RMS is objectively large and the frozen-evaluator after-RMS of
// the v4 geometry must drop strictly below 0.9x (the D5 gate the engine stage
// enforces). GA2 proves D6 determinism (same input + same pinned seed =
// bitwise-equal v4 payload + configuration hash). GA3/GA4 prove fail-closed
// validation (missing seed; unsupported camera models). GA5 proves fixed
// intrinsics are reproduced byte-identically on an opencv_radial (non-k3)
// camera. GA6 proves the vacuous empty-observation case is a well-defined no-op.
//
// Same GTSAM/Boost registration constraints as test_gtsam_adapter.cpp: GTest
// first, own main(), manual add_test (no discovery). No engine is linked —
// this drives the adapter exactly as the engine stage would.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "adapters/gtsam/gtsam_bundle_adjustment_optimizer.h"
#include "core/errors/project_error.h"
#include "core/geometry/camera_model.h"
#include "core/geometry/reprojection.h"
#include "core/geometry/se3.h"
#include "core/geometry/triangulation.h"
#include "core/reconstruction/reconstruction.h"
#include "core/utils/uuid.h"

namespace {

using nlohmann::json;
using spatial::adapters::gtsam::GtsamBundleAdjustmentOptimizer;
using spatial::core::ReconCamera;
using spatial::core::ReconImage;
using spatial::core::ReconPoint3D;
using spatial::core::Reconstruction;
using spatial::core::ValidationError;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::CameraModel;
using spatial::core::geometry::CameraModelKind;
using spatial::core::geometry::CameraView;
using spatial::core::geometry::ReprojectionObservation;
using spatial::core::geometry::SE3;
using spatial::core::geometry::TriangulationCandidate;
using spatial::core::geometry::TriangulateTwoRays;
using spatial::core::geometry::Quaternion;

constexpr double kPi = 3.14159265358979323846;
constexpr double kFx = 800.0, kCx = 320.0, kCy = 240.0;
constexpr double kNoisePx = 0.0;

Eigen::Matrix3d GroundTruthRotation() {
  const Eigen::AngleAxisd yaw(12.0 * kPi / 180.0, Eigen::Vector3d::UnitY());
  const Eigen::AngleAxisd pitch(-8.0 * kPi / 180.0, Eigen::Vector3d::UnitX());
  const Eigen::AngleAxisd roll(5.0 * kPi / 180.0, Eigen::Vector3d::UnitZ());
  return (yaw * pitch * roll).toRotationMatrix();
}

Eigen::Vector3d GroundTruthTranslation() { return {0.6, -0.2, 0.15}; }

// True reconstruction-frame pose of the target camera (world-from-camera).
Eigen::Matrix3d TrueTargetRotation() {
  return GroundTruthRotation().transpose();
}

Eigen::Vector3d TrueTargetTranslation() {
  return -GroundTruthRotation().transpose() * GroundTruthTranslation();
}

std::array<double, 4> QuatXyzw(const Eigen::Matrix3d& R) {
  const Eigen::Quaterniond q(R);
  return {q.x(), q.y(), q.z(), q.w()};
}

ReconCamera PinholeCamera(std::uint32_t id) {
  ReconCamera cam;
  cam.camera_id = id;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsic_model = "pinhole";
  cam.fx = kFx;
  cam.fy = kFx;
  cam.cx = kCx;
  cam.cy = kCy;
  cam.distortion_model = "none";
  return cam;
}

// Renders n_points from the true geometry: the source camera at the identity
// pose at the world origin, the target at its TRUE reconstruction-frame pose.
// Pixels are the exact rendered projections (kNoisePx = 0).
struct Scene {
  std::vector<Eigen::Vector3d> X;
  std::vector<std::pair<double, double>> src;
  std::vector<std::pair<double, double>> tgt;
};

Scene RenderScene(int n_points, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> ux(-2.0, 2.0);
  std::uniform_real_distribution<double> uy(-1.5, 1.5);
  std::uniform_real_distribution<double> uz(3.0, 8.0);
  Scene scene;
  const Eigen::Matrix3d R = GroundTruthRotation();
  const Eigen::Vector3d t = GroundTruthTranslation();
  while (static_cast<int>(scene.X.size()) < n_points) {
    const Eigen::Vector3d X(ux(rng), uy(rng), uz(rng));
    const Eigen::Vector3d Xt = R * X + t;
    if (Xt.z() < 0.5) continue;
    const double sx = kFx * X.x() / X.z() + kCx;
    const double sy = kFx * X.y() / X.z() + kCy;
    const double tx = kFx * Xt.x() / Xt.z() + kCx;
    const double ty = kFx * Xt.y() / Xt.z() + kCy;
    if (sx < 8.0 || sx > 632.0 || sy < 8.0 || sy > 472.0) continue;
    if (tx < 8.0 || tx > 632.0 || ty < 8.0 || ty > 472.0) continue;
    scene.X.push_back(X);
    scene.src.push_back({sx, sy});
    scene.tgt.push_back({tx, ty});
  }
  return scene;
}

// Builds the exact observation set: every point is seen in both images at its
// rendered pixel (matching what the canonical feature chain resolves).
std::vector<ReprojectionObservation> Observations(const Scene& scene) {
  std::vector<ReprojectionObservation> obs;
  for (std::size_t i = 0; i < scene.X.size(); ++i) {
    ReprojectionObservation s;
    s.image_id = 1u;
    s.point3d_id = static_cast<std::uint64_t>(i + 1);
    s.keypoint_2d = Eigen::Vector2d(scene.src[i].first, scene.src[i].second);
    obs.push_back(s);
    ReprojectionObservation t;
    t.image_id = 2u;
    t.point3d_id = static_cast<std::uint64_t>(i + 1);
    t.keypoint_2d = Eigen::Vector2d(scene.tgt[i].first, scene.tgt[i].second);
    obs.push_back(t);
  }
  return obs;
}

// The v3-style source: TRUE poses (world-from-camera), points displaced from
// the true geometry ALONG the source ray (a pure depth perturbation keeps the
// displacement unambiguous, exactly the kind of error re-triangulation leaves
// behind when pose claims are imperfect). Image 1 is anchored at the identity.
Reconstruction BuildSource(const Scene& scene) {
  Reconstruction src;
  src.reconstruction_id = "11111111-1111-4111-8111-111111111111";
  src.scene_id = "22222222-2222-4222-8222-222222222222";
  src.coordinate_frame = "trajectory_0";
  src.status = "succeeded";
  src.created_at_ns = 5000;
  src.cameras.push_back(PinholeCamera(1));
  src.cameras.push_back(PinholeCamera(2));

  ReconImage img1;
  img1.image_id = 1;
  img1.camera_id = 1;
  img1.frame_id = "33333333-3333-4333-8333-333333333333";
  img1.name = "frame_s.jpg";
  img1.pose.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  img1.pose.translation_xyz = {0.0, 0.0, 0.0};
  src.images.push_back(img1);

  ReconImage img2;
  img2.image_id = 2;
  img2.camera_id = 2;
  img2.frame_id = "44444444-4444-4444-8444-444444444444";
  img2.name = "frame_t.jpg";
  const std::array<double, 4> r = QuatXyzw(TrueTargetRotation());
  img2.pose.rotation_xyzw = r;
  const Eigen::Vector3d t = TrueTargetTranslation();
  img2.pose.translation_xyz = {t.x(), t.y(), t.z()};
  src.images.push_back(img2);

  for (std::size_t i = 0; i < scene.X.size(); ++i) {
    const Eigen::Vector3d& X = scene.X[i];
    // Transverse world-x displacement (5 cm): the ambiguity is visible in BOTH
    // views (a depth-along-ray displacement would be invisible in the source
    // projection), exactly the kind of error imperfect pose claims leave after
    // re-triangulation.
    const Eigen::Vector3d moved = X + Eigen::Vector3d(0.05, 0.0, 0.0);
    ReconPoint3D pt;
    pt.point3d_id = static_cast<std::uint64_t>(i + 1);
    pt.xyz = {moved.x(), moved.y(), moved.z()};
    pt.track = {{1u, static_cast<std::int32_t>(i)},
                {2u, static_cast<std::int32_t>(i)}};
    src.points3D.push_back(std::move(pt));
  }
  return src;
}

// Serializes the (points, poses) payload of a v4 so two runs can be compared
// bit-for-bit (json double formatting is a pure function of the bits).
json PayloadDigest(const Reconstruction& rec) {
  json out;
  json pts = json::array();
  for (const ReconPoint3D& p : rec.points3D) pts.push_back(p.xyz);
  out["points3D"] = std::move(pts);
  json poses = json::array();
  for (const ReconImage& img : rec.images) {
    poses.push_back({{"r", img.pose.rotation_xyzw},
                     {"t", img.pose.translation_xyz}});
  }
  out["images"] = std::move(poses);
  return out;
}

BundleAdjustmentInput InputFor(const Scene& scene,
                               std::optional<std::string> seed =
                                   std::optional<std::string>("step9-ga")) {
  BundleAdjustmentInput in;
  in.source = BuildSource(scene);
  in.observations = Observations(scene);
  in.random_seed = seed;
  in.max_iterations = 100;
  return in;
}

TEST(GtsamBundleAdjustmentAdapterTest,
     GA1_RealRefinementStrictlyReducesReprojection) {
  const Scene scene = RenderScene(36, 7u);
  GtsamBundleAdjustmentOptimizer opt;
  BundleAdjustmentInput in = InputFor(scene);

  // Sanity: the displaced source geometry really is wrong (mean ~3 px RMS).
  const auto views_before =
      spatial::core::geometry::InitializeReprojectionViews(in.source);
  const auto points_before =
      spatial::core::geometry::ReconstructionPoints(in.source);
  const auto metrics_before = spatial::core::geometry::EvaluateReprojection(
      views_before, points_before, in.observations);
  ASSERT_GT(metrics_before.rmse_px, 1.0);

  const BundleAdjustmentResult result = opt.optimize(in);

  EXPECT_TRUE(result.trace.converged);
  EXPECT_GT(result.trace.iterations, 0);
  // The frozen D5 gate margin the engine stage requires.
  EXPECT_LT(result.trace.rms_after_px,
            0.9 * result.trace.rms_before_px);
  EXPECT_LT(result.trace.rms_after_px, result.trace.rms_before_px);
  EXPECT_GT(result.trace.inlier_count_after, 0);

  const Reconstruction& v4 = result.reconstruction;
  EXPECT_NE(v4.reconstruction_id, in.source.reconstruction_id);
  EXPECT_EQ(v4.status, "succeeded");
  EXPECT_EQ(v4.created_at_ns, in.source.created_at_ns + 100);
  EXPECT_EQ(v4.scene_id, in.source.scene_id);
  EXPECT_EQ(v4.coordinate_frame, in.source.coordinate_frame);
  EXPECT_EQ(v4.provenance.backend.name, "spatial_gtsam_bundle_adjuster");
  EXPECT_FALSE(v4.provenance.backend.version.empty());
  EXPECT_FALSE(v4.provenance.configuration_hash.empty());
  EXPECT_EQ(v4.points3D.size(), in.source.points3D.size());

  // D3: fixed intrinsics reproduced byte-identically.
  EXPECT_EQ(v4.cameras.size(), in.source.cameras.size());
  for (std::size_t i = 0; i < v4.cameras.size(); ++i) {
    const ReconCamera& a = in.source.cameras[i];
    const ReconCamera& b = v4.cameras[i];
    EXPECT_EQ(a.camera_id, b.camera_id);
    EXPECT_EQ(a.intrinsic_model, b.intrinsic_model);
    EXPECT_EQ(a.fx, b.fx);
    EXPECT_EQ(a.fy, b.fy);
    EXPECT_EQ(a.cx, b.cx);
    EXPECT_EQ(a.cy, b.cy);
    EXPECT_EQ(a.distortion_coefficients, b.distortion_coefficients);
  }
}

TEST(GtsamBundleAdjustmentAdapterTest, GA2_SameSeedProducesBitwiseSameV4) {
  const Scene scene = RenderScene(24, 11u);
  GtsamBundleAdjustmentOptimizer opt;
  const BundleAdjustmentResult first = opt.optimize(InputFor(scene));
  const BundleAdjustmentResult second = opt.optimize(InputFor(scene));

  EXPECT_EQ(PayloadDigest(first.reconstruction),
            PayloadDigest(second.reconstruction));
  EXPECT_EQ(first.reconstruction.provenance.configuration_hash,
            second.reconstruction.provenance.configuration_hash);
  EXPECT_EQ(first.reconstruction.provenance.backend_specific_json,
            second.reconstruction.provenance.backend_specific_json);
  EXPECT_EQ(first.trace.rms_after_px, second.trace.rms_after_px);
}

TEST(GtsamBundleAdjustmentAdapterTest, GA3_MissingSeedFailsClosed) {
  const Scene scene = RenderScene(24, 3u);
  GtsamBundleAdjustmentOptimizer opt;
  BundleAdjustmentInput in = InputFor(scene, std::nullopt);
  EXPECT_THROW(opt.optimize(in), ValidationError);
}

TEST(GtsamBundleAdjustmentAdapterTest, GA4_UnsupportedCamerasFailClosed) {
  const Scene scene = RenderScene(24, 17u);
  GtsamBundleAdjustmentOptimizer opt;
  BundleAdjustmentInput in = InputFor(scene);

  // fisheye model is not representable by Cal3DS2 -> typed validation error,
  // no iteration and no v4 (D4 no-partial-results).
  {
    ReconCamera cam = in.source.cameras[1];
    cam.intrinsic_model = "opencv_fisheye";
    cam.distortion_model = "opencv_fisheye";
    cam.distortion_coefficients = {1.0, 1.0, 1.0, 1.0};
    in.source.cameras[1] = cam;
    EXPECT_THROW(opt.optimize(in), ValidationError);
  }
  // opencv_radial with a non-zero k3 is not representable either.
  {
    BundleAdjustmentInput in2 = InputFor(scene);
    ReconCamera cam = in2.source.cameras[1];
    cam.intrinsic_model = "opencv";
    cam.distortion_model = "opencv_radial";
    cam.distortion_coefficients = {0.1, -0.02, 0.001, 0.0, 0.3};
    in2.source.cameras[1] = cam;
    EXPECT_THROW(opt.optimize(in2), ValidationError);
  }
}

TEST(GtsamBundleAdjustmentAdapterTest,
     GA5_OpenCvRadialWithoutK3IsSupportedAndExact) {
  const Scene scene = RenderScene(24, 19u);
  BundleAdjustmentInput in = InputFor(scene);
  // opencv (radial-tangential) model with exactly k1,k2,p1,p2 (no k3):
  // reachable by Cal3DS2 and the v4 must reproduce the camera byte-identically.
  // Coefficients are kept small so the pinhole-rendered observations stay
  // (near-)consistent with the distorted projection — enough signal for LM, no
  // artificial outlier regime.
  ReconCamera cam = in.source.cameras[1];
  cam.intrinsic_model = "opencv";
  cam.distortion_model = "opencv_radial";
  cam.distortion_coefficients = {1e-4, -1e-5, 1e-6, 5e-7};
  in.source.cameras[1] = cam;

  GtsamBundleAdjustmentOptimizer opt;
  const BundleAdjustmentResult result = opt.optimize(in);
  EXPECT_TRUE(result.trace.converged);
  EXPECT_EQ(result.reconstruction.cameras[1].intrinsic_model, "opencv");
  EXPECT_EQ(result.reconstruction.cameras[1].distortion_coefficients,
            cam.distortion_coefficients);
}

TEST(GtsamBundleAdjustmentAdapterTest,
     GA6_EmptyObservationsIsWellDefinedNoop) {
  const Scene scene = RenderScene(24, 23u);
  BundleAdjustmentInput in = InputFor(scene);
  in.source.points3D.clear();
  in.observations.clear();

  GtsamBundleAdjustmentOptimizer opt;
  const BundleAdjustmentResult result = opt.optimize(in);
  EXPECT_EQ(result.reconstruction.points3D.size(), 0u);
  EXPECT_EQ(result.reconstruction.status, "succeeded");
  EXPECT_EQ(result.reconstruction.created_at_ns, in.source.created_at_ns + 100);
  EXPECT_EQ(result.reconstruction.cameras, in.source.cameras);
  EXPECT_EQ(result.trace.rms_before_px, 0.0);
  EXPECT_EQ(result.trace.rms_after_px, 0.0);
  EXPECT_FALSE(result.trace.converged);  // nothing to optimize
}

}  // namespace

// GTSAM's transitive Boost dependency pulls in boost_test_exec_monitor which
// expects this symbol. We define main() ourselves instead of using gtest_main
// to avoid the Boost.Test main() conflicting with GTest's.
int test_main(int, char** const) { return 0; }

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}