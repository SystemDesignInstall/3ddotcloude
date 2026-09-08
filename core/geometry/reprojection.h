#pragma once

// Canonical reprojection residual + quality evaluator (P3-impl-8a, §4.4/§4.5).
//
// This is the platform's deterministic reprojection-consistency metric path,
// reused by 8b/8c. It computes the per-observation residual
//   r_ij = || keypoint_ij - proj_ij(point_xyz) ||   (pixels, §Q15)
// where keypoint_ij is the 2D feature pixel (carried by the caller, resolved
// from the canonical frame_id -> FeatureSet -> FeatureArtifact chain) and
// proj_ij projects the 3D point through the image's camera model + world-from-
// camera pose.
//
// Outlier rule (§4.5, D5): inlier iff r_ij <= max(3 * median(|r|), 2.0 px).
// Outliers are excluded from the RMS but counted and reported. The aggregate
// is a pure, deterministic function of its inputs (ADR-020) — no wall clock.
//
// The quality gate (RMS_after < 0.9 * RMS_before) is exposed as a REUSABLE
// pure predicate (PassesReprojectionGate / ReprojectionGate) for later 8b/8c
// orchestration. It performs no reconstruction writes and creates no revision.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <Eigen/Geometry>

#include "core/errors/project_error.h"
#include "core/geometry/camera_model.h"
#include "core/geometry/camera_transform.h"
#include "core/geometry/se3.h"
#include "core/reconstruction/reconstruction.h"

namespace spatial::core::geometry {

// A single 2D -> 3D observation. keypoint_2d is the image-space feature pixel
// (px), resolved from the FeatureArtifact at the call site — it is never
// stored inline in the Reconstruction (§4.10/§4.11).
struct ReprojectionObservation {
  std::uint32_t image_id = 0;
  std::uint64_t point3d_id = 0;
  Eigen::Vector2d keypoint_2d{};
};

// One image's projection context: its camera model and world-from-camera pose
// (T_reconstruction_camera, §4.8). Building these from a Reconstruction is
// provided by InitializeReprojectionViews.
struct CameraView {
  CameraView(std::uint32_t image_id_in, CameraModel model_in,
             SE3 world_from_camera_in)
      : image_id(image_id_in),
        model(std::move(model_in)),
        world_from_camera(std::move(world_from_camera_in)) {}

  std::uint32_t image_id = 0;
  CameraModel model;
  SE3 world_from_camera;
};

// Per-observation outcome.
struct ReprojectionResidual {
  std::uint32_t image_id = 0;
  std::uint64_t point3d_id = 0;
  std::int64_t point2d_idx = 0;
  double residual_px = 0.0;   // r_ij
  bool inlier = false;        // D5 classification
};

// Per-image aggregate (additive key in the QualityReport reprojection group).
struct PerImageReprojection {
  std::uint32_t image_id = 0;
  double rmse_px = 0.0;
  double mean_error_px = 0.0;
  std::int64_t total_count = 0;
  std::int64_t inlier_count = 0;
  std::int64_t outlier_count = 0;
};

// Per-point aggregate: mean inlier residual (matches ReconPoint3D.error
// semantics, reconstruction.h:65) plus the observation counts.
struct PerPointReprojection {
  std::uint64_t point3d_id = 0;
  double error_px = 0.0;       // mean of inlier residuals
  std::int64_t total_count = 0;
  std::int64_t inlier_count = 0;
};

// Deterministic aggregate metrics (P3-impl-8a §4.4/§4.5). rmse_px/mean_error_px
// are derived from INLIERS only; outliers are counted. median/threshold follow
// the D5 rule. Plain data (no Eigen members) so it can be carried through the
// engine QualityReport.reprojection group untouched.
struct ReprojectionMetrics {
  double rmse_px = 0.0;
  double mean_error_px = 0.0;
  double median_error_px = 0.0;
  double threshold_px = 0.0;     // max(3*median, 2.0 px), D5
  std::int64_t total_count = 0;
  std::int64_t inlier_count = 0;
  std::int64_t outlier_count = 0;
  std::vector<PerImageReprojection> per_image;
  std::vector<PerPointReprojection> per_point;
};

// The D5 inlier rule: inlier iff r <= max(3 * median(|r|), 2.0 px).
inline double ReprojectionThreshold(double median_error_px) {
  const double threshold = 3.0 * median_error_px;
  return threshold > 2.0 ? threshold : 2.0;
}

// Builds the ordered projection views for a Reconstruction. For every image
// whose camera is a first-class model, a CameraView is produced; a non-first-
// class camera FAILS CLOSED here with a typed validation error (propagated
// from CameraModel::FromReconCamera). Returns an empty vector if there are no
// images. Deterministic: views are ordered by image_id.
inline std::vector<CameraView> InitializeReprojectionViews(
    const Reconstruction& rec) {
  std::vector<CameraView> views;
  views.reserve(rec.images.size());
  for (const ReconImage& image : rec.images) {
    const ReconCamera* camera = nullptr;
    for (const ReconCamera& c : rec.cameras) {
      if (c.camera_id == image.camera_id) {
        camera = &c;
        break;
      }
    }
    if (camera == nullptr) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "reprojection: image references unknown camera_id");
    }
    views.push_back(CameraView(
        image.image_id,
        CameraModel::FromReconCamera(*camera),  // fail-closed
        SE3(Quaternion(image.pose.rotation_xyzw[0],
                       image.pose.rotation_xyzw[1],
                       image.pose.rotation_xyzw[2],
                       image.pose.rotation_xyzw[3])
                .Normalized(),
            Eigen::Vector3d(image.pose.translation_xyz[0],
                            image.pose.translation_xyz[1],
                            image.pose.translation_xyz[2]))));
  }
  return views;
}

// Resolves a reconstruction-frame point into the camera frame of a view and
// projects it, returning the pixel (px). Fail-closed: throws a typed
// ValidationError if the projection is geometrically impossible (behind
// camera) or non-finite (see CameraModel::Project).
inline Eigen::Vector2d ProjectPoint(const CameraView& view,
                                    const std::array<double, 3>& xyz) {
  const CameraFromWorld cr = WorldFromCamera(view.world_from_camera).Inverse();
  const Eigen::Vector3d p_r(xyz[0], xyz[1], xyz[2]);
  const Eigen::Vector3d p_c = cr.TransformPoint(p_r);
  return view.model.Project(p_c);
}

// Computes the residual for a single observation. Throws a typed ValidationError
// when the observation's point/image references are not resolvable or the
// projection fails (fail-closed; D4: no partial results).
inline double ComputeObservationResidual(const CameraView& view,
                                         const std::array<double, 3>& xyz,
                                         const Eigen::Vector2d& keypoint) {
  if (!keypoint.allFinite()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "reprojection: non-finite keypoint coordinate");
  }
  const Eigen::Vector2d projected = ProjectPoint(view, xyz);
  if (!projected.allFinite()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "reprojection: non-finite projected pixel");
  }
  const Eigen::Vector2d delta = keypoint - projected;
  return delta.norm();
}

// Evaluates reprojection metrics over a full observation set. Views and points
// are expected to be complete: every observation's image_id and point3d_id must
// resolve or a typed ValidationError is thrown BEFORE any metric is produced
// (fail-closed, D4). Observations are evaluated in the caller's order and, when
// `residuals_out` is provided, the per-observation residuals are returned in
// that same order for determinism.
inline ReprojectionMetrics EvaluateReprojection(
    const std::vector<CameraView>& views,
    const std::vector<std::pair<std::uint64_t, std::array<double, 3>>>& points,
    const std::vector<ReprojectionObservation>& observations,
    std::vector<ReprojectionResidual>* residuals_out = nullptr) {
  // Resolve the inputs first; any missing / non-finite reference fails closed
  // BEFORE metrics are produced (D4: no partial reports).
  const auto find_view = [&views](std::uint32_t image_id) -> const CameraView* {
    for (const CameraView& v : views) {
      if (v.image_id == image_id) return &v;
    }
    return nullptr;
  };
  const auto find_point =
      [&points](std::uint64_t point3d_id) -> const std::array<double, 3>* {
    for (const auto& p : points) {
      if (p.first == point3d_id) return &p.second;
    }
    return nullptr;
  };

  // 1. Residuals for every observation, in caller order (deterministic).
  ReprojectionMetrics metrics;
  std::vector<double> raw_residuals;
  raw_residuals.reserve(observations.size());
  std::vector<ReprojectionResidual> residuals;
  if (residuals_out != nullptr) {
    residuals.reserve(observations.size());
  }
  for (const ReprojectionObservation& obs : observations) {
    const CameraView* view = find_view(obs.image_id);
    if (view == nullptr) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "reprojection: observation references unknown image_id");
    }
    const std::array<double, 3>* xyz = find_point(obs.point3d_id);
    if (xyz == nullptr) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "reprojection: observation references unknown point3d_id");
    }
    const double r = ComputeObservationResidual(*view, *xyz, obs.keypoint_2d);
    raw_residuals.push_back(r);
    if (residuals_out != nullptr) {
      ReprojectionResidual rr;
      rr.image_id = obs.image_id;
      rr.point3d_id = obs.point3d_id;
      rr.point2d_idx = 0;
      rr.residual_px = r;
      residuals.push_back(rr);
    }
  }

  // 2. Median -> D5 threshold (3 * median, floored at 2 px).
  std::vector<double> sorted = raw_residuals;
  std::sort(sorted.begin(), sorted.end());
  double median = 0.0;
  if (!sorted.empty()) {
    const std::size_t n = sorted.size();
    median = (n % 2 == 0) ? 0.5 * (sorted[n / 2 - 1] + sorted[n / 2])
                          : sorted[n / 2];
  }
  metrics.median_error_px = median;
  metrics.threshold_px = ReprojectionThreshold(median);
  metrics.total_count = static_cast<std::int64_t>(raw_residuals.size());

  // 3. Classify inliers/outliers and build per-image / per-point aggregates.
  //    (D5: outliers excluded from RMS, but counted and reported.)
  std::vector<double> inlier_residuals;
  inlier_residuals.reserve(raw_residuals.size());
  for (std::size_t i = 0; i < raw_residuals.size(); ++i) {
    const bool inlier = raw_residuals[i] <= metrics.threshold_px;
    if (inlier) {
      inlier_residuals.push_back(raw_residuals[i]);
      ++metrics.inlier_count;
    } else {
      ++metrics.outlier_count;
    }
    if (residuals_out != nullptr) {
      residuals[i].inlier = inlier;
    }
  }

  // 4. Aggregate RMS/mean over inliers only (D4/D5).
  double sq_sum = 0.0;
  double sum = 0.0;
  for (const double r : inlier_residuals) {
    sq_sum += r * r;
    sum += r;
  }
  metrics.mean_error_px = inlier_residuals.empty()
                              ? 0.0
                              : sum / static_cast<double>(inlier_residuals.size());
  metrics.rmse_px = inlier_residuals.empty()
                        ? 0.0
                        : std::sqrt(sq_sum /
                                    static_cast<double>(inlier_residuals.size()));

  // 5. Per-image aggregates (ordered by image_id) and per-point aggregates
  //    (ordered by point3d_id); deterministic order.
  {
    std::vector<std::uint32_t> image_ids;
    for (const ReprojectionObservation& obs : observations) {
      if (std::find(image_ids.begin(), image_ids.end(), obs.image_id) ==
          image_ids.end()) {
        image_ids.push_back(obs.image_id);
      }
    }
    std::sort(image_ids.begin(), image_ids.end());
    for (const std::uint32_t image_id : image_ids) {
      PerImageReprojection agg;
      agg.image_id = image_id;
      double sq = 0.0;
      double sum_r = 0.0;
      for (std::size_t i = 0; i < observations.size(); ++i) {
        if (observations[i].image_id != image_id) continue;
        ++agg.total_count;
        const double r = raw_residuals[i];
        const bool inlier = r <= metrics.threshold_px;
        if (inlier) {
          ++agg.inlier_count;
          sq += r * r;
          sum_r += r;
        } else {
          ++agg.outlier_count;
        }
      }
      if (agg.inlier_count > 0) {
        agg.rmse_px = std::sqrt(sq / static_cast<double>(agg.inlier_count));
        agg.mean_error_px = sum_r / static_cast<double>(agg.inlier_count);
      }
      metrics.per_image.push_back(agg);
    }
  }
  {
    std::vector<std::uint64_t> point_ids;
    for (const ReprojectionObservation& obs : observations) {
      if (std::find(point_ids.begin(), point_ids.end(), obs.point3d_id) ==
          point_ids.end()) {
        point_ids.push_back(obs.point3d_id);
      }
    }
    std::sort(point_ids.begin(), point_ids.end());
    for (const std::uint64_t point_id : point_ids) {
      PerPointReprojection agg;
      agg.point3d_id = point_id;
      double sum_r = 0.0;
      for (std::size_t i = 0; i < observations.size(); ++i) {
        if (observations[i].point3d_id != point_id) continue;
        ++agg.total_count;
        const bool inlier = raw_residuals[i] <= metrics.threshold_px;
        if (inlier) {
          ++agg.inlier_count;
          sum_r += raw_residuals[i];
        }
      }
      if (agg.inlier_count > 0) {
        agg.error_px = sum_r / static_cast<double>(agg.inlier_count);
      }
      metrics.per_point.push_back(agg);
    }
  }

  if (residuals_out != nullptr) {
    *residuals_out = std::move(residuals);
  }
  return metrics;
}

// ---- Reusable quality gate (D5) -------------------------------------------
//
// Pure predicate and metric for 8b/8c orchestration. It does NOT create a
// Reconstruction revision, does not supersede anything, and performs no DB
// writes. rms_after and rms_before must be finite and non-negative; the gate
// is RMS_after < improvement_factor * RMS_before with a frozen default of 0.9
// (10% minimum margin, §4.5).

// Holds the gate's frozen default configuration (10% minimum margin).
struct ReprojectionGate {
  double improvement_factor = 0.9;  // frozen default (D5)
};

// Passes the gate when the after-RMS is strictly below the required improvement
// fraction of the before-RMS. Returns false (gate fails) for non-finite or
// negative inputs — a worse / undefined reconstruction never passes.
inline bool PassesReprojectionGate(double rms_after, double rms_before,
                                   double improvement_factor = 0.9) {
  if (!std::isfinite(rms_after) || !std::isfinite(rms_before)) return false;
  if (rms_after < 0.0 || rms_before < 0.0) return false;
  if (improvement_factor <= 0.0) return false;
  return rms_after < improvement_factor * rms_before;
}

}  // namespace spatial::core::geometry
