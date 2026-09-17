// P3.1 Step 2: EssentialGeometricVerifier tests (T1–T12).
//
// Fixture honesty rule: the synthetic generator knows the ground-truth pose,
// the PROVIDER never sees it — it receives only pixels + ReconCamera. The
// tests compare the provider's R/t_hat output against the hidden generator
// pose. Links core + the visual-geometry adapter only (no GTSAM, no engine).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "adapters/visual_geometry/essential_verifier.h"
#include "core/errors/project_error.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/loop_closure/geometric_verifier.h"
#include "core/loop_closure/verification_options.h"
#include "core/reconstruction/reconstruction.h"
#include "core/trajectory/pose_graph_helpers.h"
#include "core/trajectory/trajectory.h"

namespace spatial::adapters::visual_geometry {
namespace {

using spatial::core::ErrorCode;
using spatial::core::FeatureCorrespondence;
using spatial::core::FeatureKeypoint;
using spatial::core::GeometricVerificationOptions;
using spatial::core::LoopClosureCandidate;
using spatial::core::LoopClosureVerificationInput;
using spatial::core::MatchingFrameDescriptors;
using spatial::core::MetricBasis;
using spatial::core::MetricLoopClosureMeasurement;
using spatial::core::ReconCamera;
using spatial::core::TrajectoryPoseNode;

constexpr double kFx = 800.0;
constexpr double kFy = 800.0;
constexpr double kCx = 320.0;
constexpr double kCy = 240.0;
constexpr double kPi = 3.14159265358979323846;

ReconCamera TestCamera() {
  ReconCamera cam;
  cam.camera_id = 0;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsic_model = "pinhole";
  cam.fx = kFx;
  cam.fy = kFy;
  cam.cx = kCx;
  cam.cy = kCy;
  cam.distortion_model = "none";
  return cam;
}

// Non-symmetric ground-truth rotation (12 deg yaw, -8 deg pitch, 5 deg roll):
// far from symmetric, so R vs R^T are unmistakable (T2).
Eigen::Matrix3d GroundTruthRotation() {
  const Eigen::AngleAxisd yaw(12.0 * kPi / 180.0, Eigen::Vector3d::UnitY());
  const Eigen::AngleAxisd pitch(-8.0 * kPi / 180.0, Eigen::Vector3d::UnitX());
  const Eigen::AngleAxisd roll(5.0 * kPi / 180.0, Eigen::Vector3d::UnitZ());
  return (yaw * pitch * roll).toRotationMatrix();
}

// Off-axis ground-truth translation in the SOURCE frame: +t vs -t and the
// source-frame expression are all distinguishable (T3).
Eigen::Vector3d GroundTruthTranslation() { return {0.6, -0.2, 0.15}; }

// The fixture renders X_t = R_gt * X_s + t_gt (the EPIPOLAR/backend convention)
// with the source camera at the world origin and identity rotation. The target
// camera's canonical world-from-camera pose (T_reconstruction_camera, §4.8) is
// therefore R_wt = R_gt^T and t_wt = -R_gt^T * t_gt (its camera centre).
Eigen::Matrix3d TrueTargetRotation() {
  return GroundTruthRotation().transpose();
}

Eigen::Vector3d TrueTargetTranslation() {
  return -GroundTruthRotation().transpose() * GroundTruthTranslation();
}

// The EXPECTED canonical provider output, computed with the platform's own
// frozen definition of a relative transform (T_source_target = Ta^-1 * Tb,
// pose_graph_helpers.h RelativePoseBetween) from the true world poses — not
// from backend names or hand-derived transposes.
spatial::core::RelativePose ExpectedCanonicalPose() {
  const Eigen::Vector3d t_wt = TrueTargetTranslation();
  const Eigen::Quaterniond q(TrueTargetRotation());
  TrajectoryPoseNode source;
  source.frame_id = "frame-source";
  source.position_xyz = {0.0, 0.0, 0.0};
  source.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  TrajectoryPoseNode target;
  target.frame_id = "frame-target";
  target.position_xyz = {t_wt.x(), t_wt.y(), t_wt.z()};
  target.rotation_xyzw = {q.x(), q.y(), q.z(), q.w()};
  return spatial::core::RelativePoseBetween(source, target);
}

// Converts the provider's canonical output back to the epipolar convention
// (X_t = R_ess X_s + t_ess) for the cheirality proof, which is internal to
// the decomposition.
void CanonicalToEpipolar(const Eigen::Matrix3d& R_st,
                         const Eigen::Vector3d& t_st, Eigen::Matrix3d* R_ess,
                         Eigen::Vector3d* t_ess) {
  *R_ess = R_st.transpose();
  *t_ess = -R_st.transpose() * t_st;
}

Eigen::Vector2d Project(const Eigen::Vector3d& X) {
  return {kFx * X.x() / X.z() + kCx, kFy * X.y() / X.z() + kCy};
}

MatchingFrameDescriptors MakeFeatures(
    const std::vector<std::pair<double, double>>& keypoints,
    std::int64_t ts, const std::string& frame_id) {
  MatchingFrameDescriptors f;
  f.frame_id = frame_id;
  f.timestamp_ns = ts;
  f.descriptor_type = "mock_16";
  for (std::size_t i = 0; i < keypoints.size(); ++i) {
    f.keypoints.push_back(FeatureKeypoint{keypoints[i].first,
                                          keypoints[i].second});
    std::vector<double> row(16, 0.0);
    std::uint32_t h =
        static_cast<std::uint32_t>(i + 1) * 2654435761u;
    for (auto& v : row) {
      h = h * 1664525u + 1013904223u;
      v = static_cast<double>(h >> 8) * (1.0 / 16777216.0);
    }
    f.descriptors.push_back(std::move(row));
  }
  return f;
}

std::vector<FeatureCorrespondence> Pairwise(std::size_t n) {
  std::vector<FeatureCorrespondence> corr;
  corr.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    corr.push_back(FeatureCorrespondence{
        static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i), 0.0});
  return corr;
}

struct SyntheticPair {
  MatchingFrameDescriptors src;
  MatchingFrameDescriptors tgt;
  std::vector<FeatureCorrespondence> corr;
};

// Generates N exact correspondences of random 3D points under (R_gt, t_gt).
// Corrupts the LAST `outlier_count` target observations with random pixels.
// The ground-truth pose is used ONLY to render pixels, never fed to Verify.
SyntheticPair MakeSyntheticScene(const Eigen::Matrix3d& R_gt,
                                 const Eigen::Vector3d& t_gt, int n_points,
                                 int outlier_count, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> ux(-2.0, 2.0);
  std::uniform_real_distribution<double> uy(-1.5, 1.5);
  std::uniform_real_distribution<double> uz(3.0, 8.0);
  std::uniform_real_distribution<double> upx(8.0, 632.0);
  std::uniform_real_distribution<double> upy(8.0, 472.0);

  std::vector<std::pair<double, double>> src_px, tgt_px;
  while (static_cast<int>(src_px.size()) < n_points) {
    const Eigen::Vector3d X(ux(rng), uy(rng), uz(rng));
    const Eigen::Vector3d Xt = R_gt * X + t_gt;
    if (Xt.z() < 0.5) continue;
    const Eigen::Vector2d ps = Project(X);
    const Eigen::Vector2d pt = Project(Xt);
    if (ps.x() < 8.0 || ps.x() > 632.0 || ps.y() < 8.0 || ps.y() > 472.0)
      continue;
    if (pt.x() < 8.0 || pt.x() > 632.0 || pt.y() < 8.0 || pt.y() > 472.0)
      continue;
    src_px.push_back({ps.x(), ps.y()});
    tgt_px.push_back({pt.x(), pt.y()});
  }
  for (int i = 0; i < outlier_count; ++i) {
    const std::size_t k = tgt_px.size() - 1 - static_cast<std::size_t>(i);
    tgt_px[k] = {upx(rng), upy(rng)};
  }

  SyntheticPair pair;
  pair.src = MakeFeatures(src_px, 2000, "frame-source");
  pair.tgt = MakeFeatures(tgt_px, 1000, "frame-target");
  pair.corr = Pairwise(src_px.size());
  return pair;
}

LoopClosureVerificationInput MakeInput(SyntheticPair& pair, bool with_cams) {
  LoopClosureCandidate cand;
  cand.trajectory_id = "traj-1";
  cand.candidate_id = "cand-1";
  cand.source_frame_id = pair.src.frame_id;
  cand.target_frame_id = pair.tgt.frame_id;
  cand.feature_match_score = 999.0;  // never drives acceptance
  LoopClosureVerificationInput in;
  in.candidate = cand;
  in.source = pair.src;
  in.target = pair.tgt;
  in.correspondences = pair.corr;
  if (with_cams) {
    in.source_camera = TestCamera();
    in.target_camera = TestCamera();
  }
  return in;
}

Eigen::Matrix3d ResultRotation(
    const spatial::core::GeometricVerificationResult& res) {
  const auto& q = res.unit_relative_pose->rotation_xyzw;
  const Eigen::Quaterniond eq(q[3], q[0], q[1], q[2]);
  return eq.normalized().toRotationMatrix();
}

Eigen::Vector3d ResultDirection(
    const spatial::core::GeometricVerificationResult& res) {
  const auto& t = res.unit_relative_pose->translation_direction_xyz;
  return {t[0], t[1], t[2]};
}

double RotationAngle(const Eigen::Matrix3d& A, const Eigen::Matrix3d& B) {
  const double trace = (A.transpose() * B).trace();
  const double clamped = std::max(-1.0, std::min(1.0, (trace - 1.0) * 0.5));
  return std::acos(clamped);
}

// Independent SVD-based linear triangulation (DLT nullspace — a different
// method from the adapter's normal-equation solver) for the T4 proof.
int CountFrontIndependent(const SyntheticPair& pair, const Eigen::Matrix3d& R,
                          const Eigen::Vector3d& t) {
  Eigen::Matrix<double, 3, 4> P1 = Eigen::Matrix<double, 3, 4>::Zero();
  P1.block<3, 3>(0, 0) = Eigen::Matrix3d::Identity();
  Eigen::Matrix<double, 3, 4> P2 = Eigen::Matrix<double, 3, 4>::Zero();
  P2.block<3, 3>(0, 0) = R;
  P2.block<3, 1>(0, 3) = t;
  int front = 0;
  for (std::size_t i = 0; i < pair.corr.size(); ++i) {
    const auto& cs = pair.src.keypoints[pair.corr[i].source_index];
    const auto& ct = pair.tgt.keypoints[pair.corr[i].target_index];
    const double xs = (cs.x - kCx) / kFx;
    const double ys = (cs.y - kCy) / kFy;
    const double xt = (ct.x - kCx) / kFx;
    const double yt = (ct.y - kCy) / kFy;
    Eigen::Matrix<double, 4, 4> A;
    A.row(0) = xs * P1.row(2) - P1.row(0);
    A.row(1) = ys * P1.row(2) - P1.row(1);
    A.row(2) = xt * P2.row(2) - P2.row(0);
    A.row(3) = yt * P2.row(2) - P2.row(1);
    const Eigen::JacobiSVD<Eigen::Matrix<double, 4, 4>> svd(
        A, Eigen::ComputeFullV);
    const Eigen::Vector4d Xh = svd.matrixV().col(3);
    if (std::abs(Xh.w()) < 1e-12) continue;
    const Eigen::Vector3d X = Xh.head<3>() / Xh.w();
    if (!X.allFinite()) continue;
    const Eigen::Vector3d Xt = R * X + t;
    if (X.z() > 0.0 && Xt.z() > 0.0) ++front;
  }
  return front;
}

// T1 — success path: exact synthetic scene -> verified essential unit pose in
// the platform's canonical T_source_target convention.
TEST(EssentialVerifier, T1_SuccessPath) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  const Eigen::Vector3d t_gt = GroundTruthTranslation();
  SyntheticPair pair = MakeSyntheticScene(R_gt, t_gt, 48, 0, 42u);
  LoopClosureVerificationInput in = MakeInput(pair, true);

  EssentialGeometricVerifier verifier;
  const auto res = verifier.Verify(in, GeometricVerificationOptions{});

  EXPECT_TRUE(res.verified);
  EXPECT_EQ(res.geometric_model, "essential");
  EXPECT_EQ(res.rejection_reason, "");
  ASSERT_TRUE(res.unit_relative_pose.has_value());
  EXPECT_EQ(res.inlier_count, 48u);
  EXPECT_DOUBLE_EQ(res.inlier_ratio, 1.0);
  EXPECT_NEAR(ResultDirection(res).norm(), 1.0, 1e-12);

  const spatial::core::RelativePose expected = ExpectedCanonicalPose();
  const Eigen::Quaterniond qe(expected.rotation[3], expected.rotation[0],
                              expected.rotation[1], expected.rotation[2]);
  EXPECT_LT(RotationAngle(ResultRotation(res), qe.toRotationMatrix()), 1e-6);
  const Eigen::Vector3d t_hat_expected =
      Eigen::Vector3d(expected.position[0], expected.position[1],
                      expected.position[2])
          .normalized();
  EXPECT_GT(ResultDirection(res).dot(t_hat_expected), 1.0 - 1e-9);
}

// T2 — rotation convention: R must equal the canonical T_source_target
// rotation (RelativePoseBetween of the true poses), provably NOT the raw
// epipolar R_ess (its transpose).
TEST(EssentialVerifier, T2_RotationConvention) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  const Eigen::Vector3d t_gt = GroundTruthTranslation();
  SyntheticPair pair = MakeSyntheticScene(R_gt, t_gt, 48, 0, 42u);
  LoopClosureVerificationInput in = MakeInput(pair, true);

  EssentialGeometricVerifier verifier;
  const auto res = verifier.Verify(in, GeometricVerificationOptions{});
  ASSERT_TRUE(res.verified);
  ASSERT_TRUE(res.unit_relative_pose.has_value());

  const Eigen::Matrix3d R_est = ResultRotation(res);
  const Eigen::Matrix3d R_canonical = TrueTargetRotation();  // R_ess^T
  EXPECT_LT(RotationAngle(R_est, R_canonical), 1e-6);
  // Exporting the backend/epipolar rotation directly (R_ess = R_gt) would be a
  // large, detectable error for this non-symmetric ground truth.
  EXPECT_GT(RotationAngle(R_est, R_gt), 0.3);
}

// T3 — translation convention: the canonical T_source_target direction in the
// source frame, provably NOT the raw epipolar translation and not its negation.
TEST(EssentialVerifier, T3_TranslationConvention) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  const Eigen::Vector3d t_gt = GroundTruthTranslation();
  SyntheticPair pair = MakeSyntheticScene(R_gt, t_gt, 48, 0, 42u);
  LoopClosureVerificationInput in = MakeInput(pair, true);

  EssentialGeometricVerifier verifier;
  const auto res = verifier.Verify(in, GeometricVerificationOptions{});
  ASSERT_TRUE(res.verified);
  ASSERT_TRUE(res.unit_relative_pose.has_value());

  // Canonical: t_st = R_ws^T (c_t - c_s) = c_t here (source at the origin).
  const Eigen::Vector3d t_hat_canonical = TrueTargetTranslation().normalized();
  EXPECT_GT(ResultDirection(res).dot(t_hat_canonical), 1.0 - 1e-9);
  EXPECT_LT(ResultDirection(res).dot(-t_hat_canonical), -1.0 + 1e-9);
  // The raw epipolar translation (t_ess = t_gt) points the OTHER way for this
  // fixture: exporting it unchanged would invert the loop constraint.
  EXPECT_LT(ResultDirection(res).dot(t_gt.normalized()), -0.9);
}

// T4 — cheirality: the winning decomposition puts points in front of BOTH
// cameras (Z_s > 0, Z_t > 0); the sign-flipped candidate does not.
TEST(EssentialVerifier, T4_CheiralitySelection) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  const Eigen::Vector3d t_gt = GroundTruthTranslation();
  SyntheticPair pair = MakeSyntheticScene(R_gt, t_gt, 48, 0, 42u);
  LoopClosureVerificationInput in = MakeInput(pair, true);

  EssentialGeometricVerifier verifier;
  const auto res = verifier.Verify(in, GeometricVerificationOptions{});
  ASSERT_TRUE(res.verified);
  ASSERT_TRUE(res.unit_relative_pose.has_value());

  // Cheirality is a property of the decomposition, i.e. of the epipolar
  // convention; convert the canonical export back before counting depths.
  Eigen::Matrix3d R_ess;
  Eigen::Vector3d t_ess;
  CanonicalToEpipolar(ResultRotation(res), ResultDirection(res), &R_ess,
                      &t_ess);
  const int n = static_cast<int>(pair.corr.size());
  const int front = CountFrontIndependent(pair, R_ess, t_ess);
  EXPECT_GE(front, static_cast<int>(0.9 * n)) << "front=" << front;
  const int front_flipped = CountFrontIndependent(pair, R_ess, -t_ess);
  EXPECT_LE(front_flipped, static_cast<int>(0.1 * n))
      << "flipped=" << front_flipped;
  // Sanity: the epipolar pair really is the fixture's ground truth.
  EXPECT_LT(RotationAngle(R_ess, R_gt), 1e-6);
  EXPECT_GT(t_ess.normalized().dot(t_gt.normalized()), 1.0 - 1e-9);
}

// T5 — insufficient correspondences fail closed.
TEST(EssentialVerifier, T5_InsufficientCorrespondences) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  SyntheticPair pair =
      MakeSyntheticScene(R_gt, GroundTruthTranslation(), 5, 0, 42u);
  LoopClosureVerificationInput in = MakeInput(pair, true);

  EssentialGeometricVerifier verifier;
  const auto res = verifier.Verify(in, GeometricVerificationOptions{});

  EXPECT_FALSE(res.verified);
  EXPECT_EQ(res.rejection_reason, "insufficient_correspondences");
  EXPECT_FALSE(res.unit_relative_pose.has_value());
  EXPECT_FALSE(res.has_relative_pose);
}

// T6 — missing calibration fails closed (both absent, and one-sided).
TEST(EssentialVerifier, T6_MissingCalibration) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  SyntheticPair pair =
      MakeSyntheticScene(R_gt, GroundTruthTranslation(), 48, 0, 42u);

  EssentialGeometricVerifier verifier;
  {
    LoopClosureVerificationInput in = MakeInput(pair, false);
    const auto res = verifier.Verify(in, GeometricVerificationOptions{});
    EXPECT_FALSE(res.verified);
    EXPECT_EQ(res.rejection_reason, "missing_calibration");
    EXPECT_FALSE(res.unit_relative_pose.has_value());
    EXPECT_FALSE(res.has_relative_pose);
  }
  {
    LoopClosureVerificationInput in = MakeInput(pair, true);
    in.target_camera.reset();  // one-sided calibration is still uncalibrated
    const auto res = verifier.Verify(in, GeometricVerificationOptions{});
    EXPECT_FALSE(res.verified);
    EXPECT_EQ(res.rejection_reason, "missing_calibration");
    EXPECT_FALSE(res.unit_relative_pose.has_value());
  }
}

// T7 — invalid calibration: unsupported model and degenerate intrinsics throw
// typed errors (fail-closed model selection BEFORE any computation).
TEST(EssentialVerifier, T7_InvalidCalibration) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  SyntheticPair pair =
      MakeSyntheticScene(R_gt, GroundTruthTranslation(), 48, 0, 42u);

  EssentialGeometricVerifier verifier;
  {
    LoopClosureVerificationInput in = MakeInput(pair, true);
    in.source_camera->intrinsic_model = "custom";  // not first-class
    EXPECT_THROW(verifier.Verify(in, GeometricVerificationOptions{}),
                 spatial::core::ProjectError);
  }
  {
    LoopClosureVerificationInput in = MakeInput(pair, true);
    in.source_camera->fx = 0.0;  // degenerate intrinsics
    EXPECT_THROW(verifier.Verify(in, GeometricVerificationOptions{}),
                 spatial::core::CalibrationError);
  }
}

// T8 — degenerate geometry: coincident points, and pure rotation (zero
// translation is unobservable) must both reject with no unit pose.
TEST(EssentialVerifier, T8_DegenerateGeometry) {
  EssentialGeometricVerifier verifier;
  {
    std::vector<std::pair<double, double>> same_src(24, {100.0, 100.0});
    std::vector<std::pair<double, double>> same_tgt(24, {200.0, 150.0});
    SyntheticPair pair;
    pair.src = MakeFeatures(same_src, 2000, "frame-source");
    pair.tgt = MakeFeatures(same_tgt, 1000, "frame-target");
    pair.corr = Pairwise(24);
    LoopClosureVerificationInput in = MakeInput(pair, true);
    const auto res = verifier.Verify(in, GeometricVerificationOptions{});
    EXPECT_FALSE(res.verified);
    EXPECT_FALSE(res.unit_relative_pose.has_value());
    EXPECT_FALSE(res.has_relative_pose);
  }
  {
    // Pure rotation: exact homography-consistent observations, t = 0.
    SyntheticPair pair = MakeSyntheticScene(GroundTruthRotation(),
                                            Eigen::Vector3d::Zero(), 48, 0,
                                            42u);
    LoopClosureVerificationInput in = MakeInput(pair, true);
    const auto res = verifier.Verify(in, GeometricVerificationOptions{});
    EXPECT_FALSE(res.verified) << "reason=" << res.rejection_reason;
    EXPECT_FALSE(res.unit_relative_pose.has_value());
    EXPECT_FALSE(res.has_relative_pose);
  }
}

// T9 — non-finite keypoints fail closed (reject, never a pose).
TEST(EssentialVerifier, T9_NonFiniteKeypoints) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  SyntheticPair pair =
      MakeSyntheticScene(R_gt, GroundTruthTranslation(), 48, 0, 42u);
  pair.src.keypoints[3].x = std::numeric_limits<double>::quiet_NaN();
  LoopClosureVerificationInput in = MakeInput(pair, true);

  EssentialGeometricVerifier verifier;
  const auto res = verifier.Verify(in, GeometricVerificationOptions{});

  EXPECT_FALSE(res.verified);
  EXPECT_FALSE(res.unit_relative_pose.has_value());
  EXPECT_FALSE(res.has_relative_pose);
}

// T10 — outlier robustness: 25% corrupted pairs still verify with the right
// pose (deterministic RANSAC proves itself).
TEST(EssentialVerifier, T10_OutlierRobustness) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  const Eigen::Vector3d t_gt = GroundTruthTranslation();
  SyntheticPair pair = MakeSyntheticScene(R_gt, t_gt, 48, 12, 7u);
  LoopClosureVerificationInput in = MakeInput(pair, true);

  EssentialGeometricVerifier verifier;
  const auto res = verifier.Verify(in, GeometricVerificationOptions{});

  EXPECT_TRUE(res.verified) << "reason=" << res.rejection_reason;
  ASSERT_TRUE(res.unit_relative_pose.has_value());
  // Looser than the exact-data T1/T3 pins: under 25% outliers a sub-degree
  // error is correct robust behaviour, not a convention failure.
  EXPECT_LT(RotationAngle(ResultRotation(res), TrueTargetRotation()), 1e-2);
  EXPECT_GT(ResultDirection(res).dot(TrueTargetTranslation().normalized()),
            1.0 - 1e-4);
  EXPECT_GE(res.inlier_count, 30u);
}

// T11 — determinism: identical inputs yield identical results (D-DI-02).
TEST(EssentialVerifier, T11_Determinism) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  SyntheticPair pair =
      MakeSyntheticScene(R_gt, GroundTruthTranslation(), 48, 12, 7u);

  EssentialGeometricVerifier verifier;
  const GeometricVerificationOptions opts;
  const auto a = verifier.Verify(MakeInput(pair, true), opts);
  const auto b = verifier.Verify(MakeInput(pair, true), opts);

  EXPECT_EQ(a.verified, b.verified);
  EXPECT_EQ(a.geometric_model, b.geometric_model);
  EXPECT_EQ(a.correspondence_count, b.correspondence_count);
  EXPECT_EQ(a.inlier_count, b.inlier_count);
  EXPECT_DOUBLE_EQ(a.inlier_ratio, b.inlier_ratio);
  EXPECT_DOUBLE_EQ(a.geometric_residual, b.geometric_residual);
  EXPECT_DOUBLE_EQ(a.confidence, b.confidence);
  EXPECT_EQ(a.rejection_reason, b.rejection_reason);
  ASSERT_TRUE(a.unit_relative_pose.has_value());
  ASSERT_TRUE(b.unit_relative_pose.has_value());
  EXPECT_EQ(a.unit_relative_pose->rotation_xyzw,
            b.unit_relative_pose->rotation_xyzw);
  EXPECT_EQ(a.unit_relative_pose->translation_direction_xyz,
            b.unit_relative_pose->translation_direction_xyz);
}

// T12 — unit/metric separation: even on success, the metric fields stay
// empty, so the frozen edge builder produces NO metric edge from them.
TEST(EssentialVerifier, T12_UnitMetricSeparation) {
  const Eigen::Matrix3d R_gt = GroundTruthRotation();
  SyntheticPair pair =
      MakeSyntheticScene(R_gt, GroundTruthTranslation(), 48, 0, 42u);
  LoopClosureVerificationInput in = MakeInput(pair, true);

  EssentialGeometricVerifier verifier;
  const auto res = verifier.Verify(in, GeometricVerificationOptions{});
  ASSERT_TRUE(res.verified);
  ASSERT_TRUE(res.unit_relative_pose.has_value());

  // The metric contract fields are untouched by the unit estimate.
  EXPECT_FALSE(res.has_relative_pose);
  EXPECT_EQ(res.relative_position_xyz,
            (std::array<double, 3>{0.0, 0.0, 0.0}));

  // Feeding exactly those metric fields to the frozen edge builder yields no
  // edge (INV-1): a unit direction can never become a 1-metre constraint.
  TrajectoryPoseNode src_node;
  src_node.frame_id = "frame-source";
  src_node.position_xyz = {0.0, 0.0, 0.0};
  src_node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  TrajectoryPoseNode tgt_node;
  tgt_node.frame_id = "frame-target";
  tgt_node.position_xyz = {5.0, 0.0, 0.0};
  tgt_node.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  MetricLoopClosureMeasurement measurement;
  measurement.position_cs = res.relative_position_xyz;
  measurement.rotation_cst = res.relative_rotation_xyzw;
  measurement.geometric_residual = res.geometric_residual;
  const auto edge = spatial::core::BuildMetricLoopClosureEdge(
      res.closure, {src_node, tgt_node}, measurement, 0, "",
      MetricBasis{});
  EXPECT_FALSE(edge.has_value());
}

}  // namespace
}  // namespace spatial::adapters::visual_geometry
