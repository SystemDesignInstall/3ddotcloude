// P3-impl-8c: GTSAM Levenberg-Marquardt Bundle Adjustment adapter.
//
// 8c production backend for the canonical ReconstructionOptimizer seam. Converts
// the v3 document + resolved observations at the adapter boundary into a GTSAM
// factor graph, runs batch LM (GenericProjectionFactor + Cal3DS2), and maps the
// refined geometry back into a fresh canonical v4 document. Fixed intrinsics
// (D3): the v4 document reproduces the v3 camera records byte-identically; only
// poses + 3D points are refined.
//
// The D5 trace (before/after RMS, mean, threshold, inlier/outlier counts) is
// computed with the FROZEN 8a evaluator (reprojection.h) on the SAME
// observation set evaluated against the v3 and v4 geometries — never with the
// optimizer's own internal error. Determinism (D6): single-threaded LM over a
// deterministically ordered factor graph anchored by a pose prior on the
// lowest image id; the pinned random_seed is validated and recorded but the
// solve itself uses no random numbers.
//
// Architecture boundary: this file (like the trajectory adapter) links
// spatial_core PUBLIC and gtsam::gtsam PRIVATE. No GTSAM type leaves this
// translation unit.

#include "adapters/gtsam/gtsam_bundle_adjustment_optimizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/make_shared.hpp>
#include <gtsam/geometry/Cal3DS2.h>
#include <gtsam/geometry/Point2.h>
#include <gtsam/geometry/Point3.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/geometry/Rot3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/linear/NoiseModel.h>
#include <gtsam/nonlinear/LevenbergMarquardtOptimizer.h>
#include <gtsam/slam/ProjectionFactor.h>  // GenericProjectionFactor (GTSAM 4.2)
#include <gtsam/nonlinear/LevenbergMarquardtParams.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/slam/PriorFactor.h>

#include <nlohmann/json.hpp>

#include "gtsam_adapter_build_info.h"
#include "core/errors/project_error.h"
#include "core/geometry/camera_model.h"
#include "core/reconstruction/reconstruction.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"

namespace spatial::adapters::gtsam {

namespace {

using spatial::core::ErrorCode;
using spatial::core::ReconCamera;
using spatial::core::ReconImage;
using spatial::core::ReconPoint3D;
using spatial::core::Reconstruction;
using spatial::core::ReconstructionProvenance;
using spatial::core::ValidationError;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::BundleAdjustmentTrace;
using spatial::core::geometry::CameraModel;
using spatial::core::geometry::CameraModelKind;
using spatial::core::geometry::EvaluateReprojection;
using spatial::core::geometry::InitializeReprojectionViews;
using spatial::core::geometry::ReconstructionPoints;
using spatial::core::geometry::ReprojectionMetrics;
using spatial::core::geometry::ReprojectionObservation;

// Deterministic factor keys. Image ids and point ids are backend-local ids of
// the source document and therefore unique within it.
::gtsam::Key PoseKey(std::uint32_t image_id) {
  return ::gtsam::Symbol('x', image_id).key();
}
::gtsam::Key PointKey(std::uint64_t point3d_id) {
  return ::gtsam::Symbol('p', point3d_id).key();
}

// P3 scalar-last (x,y,z,w) -> GTSAM scalar-first Rot3 (w,x,y,z). The source
// document stores the quaternion already normalized (ApplyOptimizedTrajectory
// guarantees it); the normalization here is defensive and idempotent.
::gtsam::Rot3 RotationP3ToGtsam(const std::array<double, 4>& xyzw) {
  return ::gtsam::Rot3(
      Eigen::Quaterniond(xyzw[3], xyzw[0], xyzw[1], xyzw[2]).normalized());
}

std::array<double, 4> RotationGtsamToP3(const ::gtsam::Rot3& rot) {
  const Eigen::Quaterniond q = rot.toQuaternion();
  return {q.x(), q.y(), q.z(), q.w()};
}

::gtsam::Pose3 PoseP3ToGtsam(const ReconImage& image) {
  return ::gtsam::Pose3(RotationP3ToGtsam(image.pose.rotation_xyzw),
                        ::gtsam::Point3(image.pose.translation_xyz[0],
                                        image.pose.translation_xyz[1],
                                        image.pose.translation_xyz[2]));
}

// The GTSAM calibration for one first-class camera. Throws a typed validation
// error when the camera is not exactly representable (fisheye/fov, or
// opencv_radial with a non-zero k3) — the seam never approximates a model.
// Cal3DS2 is the exact image of the supported models: pinhole is the zero-
// distortion specialization, opencv_radial-without-k3 is k1,k2,p1,p2 verbatim.
::gtsam::Cal3DS2::shared_ptr CalibrationFromCamera(const ReconCamera& camera) {
  // Fail-closed model selection first (unsupported intrinsic/distortion combo
  // or degenerate intrinsics are rejected before any computation).
  const CameraModel model = CameraModel::FromReconCamera(camera);
  double k1 = 0.0, k2 = 0.0, p1 = 0.0, p2 = 0.0;
  if (model.kind() == CameraModelKind::kPinhole) {
    // No distortion; Cal3DS2 with zero radial/tangential is bit-exact pinhole.
  } else if (model.kind() == CameraModelKind::kOpenCvRadial) {
    const std::vector<double>& c = camera.distortion_coefficients;
    // FromReconCamera already validated 4 or 5 coefficients.
    if (c.size() == 5u && c[4] != 0.0) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "gtsam bundle adjustment: opencv_radial k3 coefficient is not "
          "representable by Cal3DS2; the model fails closed instead of being "
          "approximated (use the COLMAP backend for k3 models)",
          /*details=*/{}, /*recoverable=*/false,
          "8c reproduces camera models exactly or not at all; the COLMAP "
          "bundle_adjuster is the production path for k3 distortion.");
    }
    k1 = c[0];
    k2 = c[1];
    p1 = c[2];
    p2 = c[3];
  } else {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "gtsam bundle adjustment: camera model '" + camera.intrinsic_model +
            "' is not representable by Cal3DS2; the model fails closed instead "
            "of being approximated (use the COLMAP backend)",
        /*details=*/{}, /*recoverable=*/false,
        "8c reproduces camera models exactly or not at all; the COLMAP "
        "bundle_adjuster is the production path for fisheye/fov models.");
  }
  return ::boost::make_shared<::gtsam::Cal3DS2>(
      camera.fx, camera.fy, /*skew=*/0.0, camera.cx, camera.cy, k1, k2, p1, p2);
}

using CameraCalibrationMap =
    std::map<std::uint32_t, ::gtsam::Cal3DS2::shared_ptr>;

CameraCalibrationMap BuildCalibrations(const Reconstruction& source) {
  CameraCalibrationMap cals;
  for (const ReconCamera& camera : source.cameras) {
    cals.emplace(camera.camera_id, CalibrationFromCamera(camera));
  }
  return cals;
}

// Effective configuration hash (D6 / ADR-020 determinism): a pure function of
// the adapter options + pinned seed so equal runs produce equal hashes and the
// v4 configuration/backend_specific fields stay reproducible.
std::string ConfigurationHash(const std::string& robust_loss,
                              double robust_loss_scale_px,
                              const std::string& seed, int max_iterations) {
  spatial::core::Sha256 hasher;
  hasher.Update("robust_loss:");
  hasher.Update(robust_loss);
  hasher.Update(";scale:");
  hasher.Update(std::to_string(robust_loss_scale_px));
  hasher.Update(";seed:");
  hasher.Update(seed);
  hasher.Update(";max_iterations:");
  hasher.Update(std::to_string(max_iterations));
  return spatial::core::Sha256Hex(hasher.Final());
}

// v4 provenance (P13): backend identity of the gtsam bundle-adjuster layer and
// the inherited input-artifact chain + the v3 reconstruction id (sorted, no
// self-reference), mirroring the COLMAP adapter. Timing stays zero (the
// determinism policy of reconstruction_feedback.h / triangulation.h).
ReconstructionProvenance BuildProvenance(const Reconstruction& source,
                                         const std::string& config_hash,
                                         const std::string& stats_json) {
  ReconstructionProvenance prov;
  prov.backend.name = "spatial_gtsam_bundle_adjuster";
  prov.backend.version = kGtsamAdapterVersion;
  prov.backend.adapter_version = kGtsamAdapterVersion;
  prov.configuration_hash = config_hash;
  prov.input_artifact_hashes = source.provenance.input_artifact_hashes;
  bool have_self = false;
  for (const std::string& h : prov.input_artifact_hashes) {
    if (h == source.reconstruction_id) have_self = true;
  }
  if (!have_self) {
    prov.input_artifact_hashes.push_back(source.reconstruction_id);
  }
  std::sort(prov.input_artifact_hashes.begin(),
            prov.input_artifact_hashes.end());
  if (!source.provenance.engine_version.empty()) {
    prov.engine_version = source.provenance.engine_version;
  }
  if (!source.provenance.engine_commit.empty()) {
    prov.engine_commit = source.provenance.engine_commit;
  }
  if (!source.provenance.git_commit.empty()) {
    prov.git_commit = source.provenance.git_commit;
  }
  prov.started_at_ns = 0;
  prov.finished_at_ns = 0;
  prov.duration_ns = 0;
  prov.backend_specific_json = stats_json;
  return prov;
}

// Mean inlier reprojection error per point from the frozen evaluator, used to
// refresh ReconPoint3D.error for points that carry observations.
std::map<std::uint64_t, double> PerPointError(
    const ReprojectionMetrics& metrics) {
  std::map<std::uint64_t, double> errors;
  for (const auto& per_point : metrics.per_point) {
    errors.emplace(per_point.point3d_id, per_point.error_px);
  }
  return errors;
}

}  // namespace

GtsamBundleAdjustmentOptimizer::GtsamBundleAdjustmentOptimizer(
    BundleAdjustmentOptions options)
    : options_(std::move(options)) {}

BundleAdjustmentResult GtsamBundleAdjustmentOptimizer::optimize(
    const BundleAdjustmentInput& input) {
  // P9/D6: a pinned seed is mandatory. Same input + same seed must give the
  // same derived metrics; the seed is validated here AND recorded in the stats.
  if (!input.random_seed || input.random_seed->empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "gtsam bundle adjustment requires a pinned non-empty random_seed "
        "(8c D6/P9)",
        /*details=*/{}, /*recoverable=*/false,
        "Set BundleAdjustmentInput.random_seed to a non-empty deterministic "
        "seed (same input + same seed must give identical derived metrics).");
  }
  const std::string& seed = *input.random_seed;

  // D5 metrics of the v3 geometry (P10). Evaluated FIRST: the frozen evaluator
  // fails closed on any non-resolvable observation reference before a single
  // GTSAM object is built (D4 no-partial-results).
  const auto views_before = InitializeReprojectionViews(input.source);
  const auto points_before = ReconstructionPoints(input.source);
  const ReprojectionMetrics before =
      EvaluateReprojection(views_before, points_before, input.observations);
  const double loss_scale =
      options_.robust_loss_scale_px > 0.0
          ? options_.robust_loss_scale_px
          : before.threshold_px;

  // Robust uniform noise model for every reprojection factor (fixed intrinsics
  // D3; Huber soft-inlier weighting at the D5-derived scale).
  const auto robust =
      ::gtsam::noiseModel::Robust::Create(
          ::gtsam::noiseModel::mEstimator::Huber::Create(loss_scale),
          ::gtsam::noiseModel::Unit::Create(2));

  const CameraCalibrationMap calibrations = BuildCalibrations(input.source);
  std::map<std::uint32_t, ::gtsam::Cal3DS2::shared_ptr> camera_by_image;
  for (const ReconImage& image : input.source.images) {
    const auto it = calibrations.find(image.camera_id);
    if (it == calibrations.end()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "gtsam bundle adjustment: image references unknown camera_id " +
              std::to_string(image.camera_id));
    }
    camera_by_image.emplace(image.image_id, it->second);
  }

  ::gtsam::NonlinearFactorGraph graph;
  ::gtsam::Values initial_values;
  for (const ReconImage& image : input.source.images) {
    initial_values.insert(PoseKey(image.image_id), PoseP3ToGtsam(image));
  }
  for (const ReconPoint3D& point : input.source.points3D) {
    initial_values.insert(
        PointKey(point.point3d_id),
        ::gtsam::Point3(point.xyz[0], point.xyz[1], point.xyz[2]));
  }

  // Measurement factors (input.observations is already validated fail-closed
  // by the v3 projection evaluation above; the same lookups must resolve here).
  for (const ReprojectionObservation& obs : input.observations) {
    const auto cam_it = camera_by_image.find(obs.image_id);
    if (cam_it == camera_by_image.end()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "gtsam bundle adjustment: observation references unknown image_id " +
              std::to_string(obs.image_id));
    }
    graph.emplace_shared<
        ::gtsam::GenericProjectionFactor<::gtsam::Pose3, ::gtsam::Point3,
                                         ::gtsam::Cal3DS2>>(
        ::gtsam::Point2(obs.keypoint_2d.x(), obs.keypoint_2d.y()), robust,
        PoseKey(obs.image_id), PointKey(obs.point3d_id), cam_it->second);
  }

  // Gauge anchor (P5): a soft prior on the LOWEST image id's pose pins the v3
  // gauge so LM refines IN PLACE instead of sloughing toward another similarity
  // of the reconstruction frame. The prior is deterministic (lowest image id).
  if (!input.source.images.empty()) {
    const ReconImage* min_image = &input.source.images.front();
    for (const ReconImage& image : input.source.images) {
      if (image.image_id < min_image->image_id) min_image = &image;
    }
    ::gtsam::Vector6 sigmas;
    sigmas << 1e-3, 1e-3, 1e-3, 1e-3, 1e-3, 1e-3;  // translation (m), rotation (rad)
    graph.add(::gtsam::PriorFactor<::gtsam::Pose3>(
        PoseKey(min_image->image_id), PoseP3ToGtsam(*min_image),
        ::gtsam::noiseModel::Diagonal::Sigmas(sigmas)));
  }

  // Batched LM with pinned parameters (D6). No random numbers are used anywhere
  // in the solve. The COLAMD ordering + fixed damping schedule keep the linear
  // solves deterministic (the same dependency discipline as the trajectory
  // adapter); no explicit thread pool is configured.
  ::gtsam::LevenbergMarquardtParams params;
  params.lambdaInitial = 1e-4;
  params.lambdaFactor = 10.0;
  params.maxIterations = input.max_iterations;
  params.relativeErrorTol = 1e-9;
  params.absoluteErrorTol = 1e-9;
  params.orderingType = ::gtsam::Ordering::COLAMD;
  ::gtsam::LevenbergMarquardtOptimizer optimizer(graph, initial_values,
                                                 params);
  // LM telemetry (mirrors the trajectory adapter's convention): the solver has
  // no converged() accessor, so the LM factor-graph error before/after (its own
  // objective, distinct from the frozen D5 metric) classifies the result — a
  // final error more than 10x the initial one (when the initial is non-zero)
  // means it diverged (checkConvergence on real data normally terminates first).
  double lm_error_initial = 0.0;
  double lm_error_final = 0.0;
  ::gtsam::Values result_values;
  try {
    lm_error_initial = graph.error(initial_values);
    result_values = optimizer.optimize();
    lm_error_final = graph.error(result_values);
  } catch (...) {
    // A failed / throw-away error evaluation leaves the trace unconverged; the
    // result values below still come from the last accepted LM iterate.
  }

  // Assemble the v4 document: fresh UUIDv4 identity (D-CRM-07), succeeded
  // status, the +100 revision-clock extension over the v3 origin, cameras
  // byte-identical (D3), refined poses + points, tracks verbatim.
  Reconstruction v4;
  v4.reconstruction_id = spatial::core::FormatUuid(spatial::core::GenerateUuid());
  v4.scene_id = input.source.scene_id;
  v4.session_ids = input.source.session_ids;
  v4.coordinate_frame = input.source.coordinate_frame;
  v4.status = "succeeded";
  v4.created_at_ns = input.source.created_at_ns + 100;
  v4.cameras = input.source.cameras;

  for (const ReconImage& image : input.source.images) {
    ReconImage refined = image;
    const ::gtsam::Pose3 pose =
        result_values.at<::gtsam::Pose3>(PoseKey(image.image_id));
    refined.pose.rotation_xyzw = RotationGtsamToP3(pose.rotation());
    const ::gtsam::Point3 t = pose.translation();
    refined.pose.translation_xyz = {t.x(), t.y(), t.z()};
    v4.images.push_back(std::move(refined));
  }
  for (const ReconPoint3D& point : input.source.points3D) {
    ReconPoint3D refined;
    refined.point3d_id = point.point3d_id;
    const ::gtsam::Point3 xyz =
        result_values.at<::gtsam::Point3>(PointKey(point.point3d_id));
    refined.xyz = {xyz.x(), xyz.y(), xyz.z()};
    refined.color = point.color;
    refined.track = point.track;
    refined.error = point.error;  // refreshed below when the point is observed
    v4.points3D.push_back(std::move(refined));
  }

  // D5 metrics of the v4 geometry over the SAME observation set (P10/P7).
  const auto views_after = InitializeReprojectionViews(v4);
  const auto points_after = ReconstructionPoints(v4);
  const ReprojectionMetrics after =
      EvaluateReprojection(views_after, points_after, input.observations);

  // Refresh each observed point's mean inlier error from the after evaluation
  // (series points with no observation keep their v3 value).
  const std::map<std::uint64_t, double> errors = PerPointError(after);
  for (ReconPoint3D& point : v4.points3D) {
    const auto it = errors.find(point.point3d_id);
    if (it != errors.end()) point.error = it->second;
  }

  // D5 trace over the SAME observation set, from the frozen evaluator only.
  BundleAdjustmentTrace trace;
  trace.converged =
      optimizer.iterations() > 0 && std::isfinite(lm_error_initial) &&
      std::isfinite(lm_error_final) &&
      !(lm_error_final > 10.0 * lm_error_initial && lm_error_initial > 0.0) &&
      std::isfinite(after.rmse_px);
  trace.iterations =
      static_cast<std::int64_t>(optimizer.iterations());
  trace.rms_before_px = before.rmse_px;
  trace.rms_after_px = after.rmse_px;
  trace.mean_before_px = before.mean_error_px;
  trace.mean_after_px = after.mean_error_px;
  trace.inlier_count_before = before.inlier_count;
  trace.outlier_count_before = before.outlier_count;
  trace.inlier_count_after = after.inlier_count;
  trace.outlier_count_after = after.outlier_count;
  trace.threshold_px_before = before.threshold_px;
  trace.threshold_px_after = after.threshold_px;

  v4.provenance = BuildProvenance(
      input.source, ConfigurationHash(options_.robust_loss, loss_scale, seed,
                                      input.max_iterations),
      nlohmann::json{
          {"optimizer", "LevenbergMarquardt"},
          {"robust_loss", options_.robust_loss},
          {"robust_loss_scale_px", loss_scale},
          {"seed", seed},
          {"iterations", trace.iterations},
          {"converged", trace.converged},
          {"rms_before", trace.rms_before_px},
          {"rms_after", trace.rms_after_px},
          {"mean_before", trace.mean_before_px},
          {"mean_after", trace.mean_after_px},
          {"inlier_count_before", trace.inlier_count_before},
          {"outlier_count_before", trace.outlier_count_before},
          {"inlier_count_after", trace.inlier_count_after},
          {"outlier_count_after", trace.outlier_count_after},
          {"threshold_px_before", trace.threshold_px_before},
          {"threshold_px_after", trace.threshold_px_after},
      }.dump());

  return {std::move(v4), std::move(trace)};
}

}  // namespace spatial::adapters::gtsam