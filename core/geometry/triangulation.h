#pragma once

// Canonical in-repo re-triangulation (P3-impl-8b, §4.10/§4.11/§4.12).
//
// Consumes a v2 Reconstruction (optimized poses from ApplyOptimizedTrajectory)
// plus pre-resolved 2D keypoint observations (resolved from the canonical
// frame_id -> FeatureSet -> FeatureArtifact -> keypoints[point2d_idx] chain).
// Produces a NEW v3 Reconstruction with refreshed 3D points and full lineage
// classification (PRESERVED / REPLACED / NEW / REJECTED).
//
// Algorithm: DLT closest-point intersection of two observation rays. For each
// v2 point whose track has >= 2 observations, the first two track elements
// define the triangulation pair. All track observations contribute to the
// acceptance predicate (reprojection residual check, D5 rule).
//
// Acceptance predicate (§4.10) — ALL four must pass:
//   1. Positive depth in BOTH triangulation cameras (w_C > 0, cheirality)
//   2. Finite XYZ
//   3. Parallax angle >= θ_min (default 2°, configurable)
//   4. Reprojection residuals satisfy D5: r <= max(3*median, 2.0 px)
//
// If ANY predicate fails -> NO ReconPoint emitted (fail-closed).
//
// Convention (§4.8): ReconImage.pose is T_reconstruction_camera (world-from-
// camera). Unproject returns a unit ray in the camera frame; the ray is
// transformed to the reconstruction (world) frame via the camera rotation.
//
// Header-only, matches core/geometry/ style. Raw Eigen confined here.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Geometry>

#include "core/errors/project_error.h"
#include "core/geometry/camera_model.h"
#include "core/geometry/camera_transform.h"
#include "core/geometry/se3.h"
#include "core/reconstruction/reconstruction.h"

namespace spatial::core::geometry {

// --- Input types ---

// A resolved 2D keypoint observation: pixel coordinate identified by
// (image_id, point2d_idx). The caller resolves these from the canonical
// frame_id -> FeatureSet -> FeatureArtifact chain; the pure function never
// touches the scene graph or CAS.
struct TriangulationObservation {
  std::uint32_t image_id = 0;
  std::int32_t point2d_idx = 0;
  Eigen::Vector2d keypoint_2d{};
};

// --- Acceptance predicate ---

// Why a candidate was accepted or rejected.
enum class TriangulationAcceptance {
  kAccepted = 0,
  kRejectedNonFinite = 1,
  kRejectedNegativeDepth = 2,
  kRejectedLowParallax = 3,
  kRejectedHighResidual = 4,
};

// --- Lineage classification (§4.11) ---

enum class PointLineage {
  kPreserved = 0,   // geometry moved < τ, original point3d_id kept
  kReplaced = 1,    // geometry moved >= τ, NEW point3d_id
  kNew = 2,         // no v2 counterpart (not used in v2->v3, future-proof)
  kRejected = 3,    // predicate failed, no point emitted
};

// Per-point lineage detail.
struct PointLineageEntry {
  std::uint64_t old_point3d_id = 0;
  std::uint64_t new_point3d_id = 0;
  PointLineage lineage = PointLineage::kRejected;
};

// Aggregate lineage counts.
struct RetriangulationLineageSummary {
  std::int64_t preserved_count = 0;
  std::int64_t replaced_count = 0;
  std::int64_t new_count = 0;
  std::int64_t rejected_count = 0;
};

// --- Options ---

struct RetriangulationOptions {
  // Minimum parallax angle in degrees between the two triangulation rays.
  // Default 2° per §4.10 item 3. Non-negotiable lower bound.
  double min_parallax_deg = 2.0;

  // Stability threshold (meters) for PRESERVED vs REPLACED classification.
  // If ||v3_xyz - v2_xyz|| < τ, the point is PRESERVED (original id kept).
  // Otherwise REPLACED (new id). Default 0.01 m (1 cm).
  double stability_threshold_m = 0.01;
};

// --- Output ---

// The raw triangulated candidate (before acceptance predicate).
struct TriangulationCandidate {
  std::array<double, 3> xyz{};
  double parallax_deg = 0.0;
  TriangulationAcceptance acceptance = TriangulationAcceptance::kRejectedNonFinite;
};

// The complete re-triangulation result: v3 reconstruction + lineage.
struct RetriangulationResult {
  core::Reconstruction reconstruction;          // v3 (new identity, new status)
  RetriangulationLineageSummary lineage;
  std::vector<PointLineageEntry> point_lineage; // per-point detail
};

// --- Core algorithm: DLT closest-point of two rays ---

// Triangulates a 3D point from two camera views and their observed pixels.
// Returns a TriangulationCandidate with the reconstructed xyz, parallax
// angle, and geometric acceptance status (cheirality + finiteness + parallax
// checked; reprojection is checked separately in the caller).
//
// The two views must have distinct image_ids. Rays are constructed by
// unprojecting each pixel to a unit ray in the camera frame, then rotating to
// the reconstruction (world) frame. The 3D point is the midpoint of the
// closest approach of the two world-frame rays (DLT / midpoint method).
inline TriangulationCandidate TriangulateTwoRays(
    const CameraView& view1, const Eigen::Vector2d& pixel1,
    const CameraView& view2, const Eigen::Vector2d& pixel2,
    double min_parallax_deg = 2.0) {
  TriangulationCandidate result;

  // 1. Unproject pixels to unit rays in camera frame.
  const Eigen::Vector3d ray_c1 = view1.model.Unproject(pixel1);
  const Eigen::Vector3d ray_c2 = view2.model.Unproject(pixel2);

  // 2. Transform rays to reconstruction (world) frame.
  //    T_rc maps camera->world; the rotation rotates camera-frame directions
  //    to world frame.
  const Eigen::Vector3d ray_w1 =
      view1.world_from_camera.rotation().Rotate(ray_c1);
  const Eigen::Vector3d ray_w2 =
      view2.world_from_camera.rotation().Rotate(ray_c2);

  // 3. Ray origins in world frame (camera positions).
  const Eigen::Vector3d origin1 = view1.world_from_camera.translation();
  const Eigen::Vector3d origin2 = view2.world_from_camera.translation();

  // 4. Closest-point of two skew lines (DLT midpoint).
  const Eigen::Vector3d e = origin2 - origin1;
  const double A = ray_w1.dot(ray_w1);
  const double B = ray_w1.dot(ray_w2);
  const double C = ray_w2.dot(ray_w2);
  const double D = ray_w1.dot(e);
  const double E = ray_w2.dot(e);
  const double denom = A * C - B * B;

  if (std::abs(denom) < 1e-12) {
    // Parallel or near-parallel rays: reject as low parallax.
    result.acceptance = TriangulationAcceptance::kRejectedLowParallax;
    return result;
  }

  const double t = (C * D - B * E) / denom;
  const double s = (B * D - A * E) / denom;

  const Eigen::Vector3d p1 = origin1 + t * ray_w1;
  const Eigen::Vector3d p2 = origin2 + s * ray_w2;
  const Eigen::Vector3d xyz = 0.5 * (p1 + p2);

  // 5. Check finite XYZ.
  if (!xyz.allFinite()) {
    result.acceptance = TriangulationAcceptance::kRejectedNonFinite;
    return result;
  }

  result.xyz = {xyz.x(), xyz.y(), xyz.z()};

  // 6. Check positive depth in BOTH cameras (cheirality, §4.8).
  const CameraFromWorld T_cr1 = WorldFromCamera(view1.world_from_camera).Inverse();
  const CameraFromWorld T_cr2 = WorldFromCamera(view2.world_from_camera).Inverse();
  const Eigen::Vector3d p_c1 = T_cr1.TransformPoint(xyz);
  const Eigen::Vector3d p_c2 = T_cr2.TransformPoint(xyz);

  if (p_c1.z() <= 0.0 || p_c2.z() <= 0.0) {
    result.acceptance = TriangulationAcceptance::kRejectedNegativeDepth;
    return result;
  }

  // 7. Check parallax angle (§4.10 item 3).
  const double cos_angle = ray_w1.normalized().dot(ray_w2.normalized());
  // Clamp for numerical safety.
  const double clamped = std::max(-1.0, std::min(1.0, cos_angle));
  const double angle_rad = std::acos(clamped);
  const double angle_deg = angle_rad * 180.0 / 3.14159265358979323846;
  result.parallax_deg = angle_deg;

  if (angle_deg < min_parallax_deg) {
    result.acceptance = TriangulationAcceptance::kRejectedLowParallax;
    return result;
  }

  // Geometric acceptance (cheirality + finiteness + parallax) passed.
  // Reprojection acceptance is checked separately against the global D5
  // threshold in the caller.
  result.acceptance = TriangulationAcceptance::kAccepted;
  return result;
}

// --- Acceptance predicate: reprojection residual check ---

// --- D5 reprojection residual computation for a single point ---

// Computes the mean reprojection error (pixels) for a candidate 3D point
// across its track observations. Returns the per-observation residuals and
// the mean. All keypoints must be finite (caller guarantees this).
inline double ComputePointReprojectionError(
    const std::array<double, 3>& xyz,
    const std::vector<Eigen::Vector3d>& camera_positions,
    const std::vector<Eigen::Matrix3d>& camera_rotations,
    const std::vector<CameraModel>& camera_models,
    const std::vector<Eigen::Vector2d>& keypoints) {
  const std::size_t n = keypoints.size();
  if (n == 0) return 0.0;

  double sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const Eigen::Vector3d p_r(xyz[0], xyz[1], xyz[2]);
    // T_cr = T_rc^{-1}. Transform point to camera frame:
    // p_c = R^T (p_r - t)
    const Eigen::Vector3d p_c =
        camera_rotations[i].transpose() * (p_r - camera_positions[i]);
    if (p_c.z() <= 0.0) {
      // Behind this camera: assign a large residual (point rejected).
      return std::numeric_limits<double>::max();
    }
    const Eigen::Vector2d projected = camera_models[i].Project(p_c);
    if (!projected.allFinite()) {
      return std::numeric_limits<double>::max();
    }
    sum += (keypoints[i] - projected).norm();
  }
  return sum / static_cast<double>(n);
}

// --- Main re-triangulation function ---

// Produces a NEW v3 Reconstruction from a v2 source with re-triangulated
// points. The observations must be pre-resolved (caller resolves from
// FeatureArtifacts via frame_id -> FeatureSet -> FeatureArtifact -> keypoints).
//
// For each v2 point with >= 2 track observations:
//   1. Triangulate from the first 2 track views
//   2. Check geometric predicates (cheirality, finiteness, parallax)
//   3. Compute reprojection residuals across ALL track views
//   4. After all points, compute global D5 threshold and accept/reject
//   5. Classify lineage (PRESERVED / REPLACED / REJECTED)
//   6. Emit accepted points into v3 with NEW point3d_id values
//
// The function does NOT write to any DB (§4.12 seam boundary, 8c/next phase).
inline RetriangulationResult Retriangulate(
    const core::Reconstruction& v2,
    const std::vector<TriangulationObservation>& observations,
    const RetriangulationOptions& options = {}) {
  // --- 1. Build CameraViews from v2 (reuse reprojection.h pattern) ---
  std::vector<CameraView> views = InitializeReprojectionViews(v2);

  // Build a view lookup by image_id.
  auto find_view = [&views](std::uint32_t image_id) -> CameraView* {
    for (CameraView& v : views) {
      if (v.image_id == image_id) return &v;
    }
    return nullptr;
  };

  // Build a keypoint lookup: (image_id, point2d_idx) -> keypoint_2d.
  struct KeypointKey {
    std::uint32_t image_id;
    std::int32_t point2d_idx;
    bool operator<(const KeypointKey& o) const {
      if (image_id != o.image_id) return image_id < o.image_id;
      return point2d_idx < o.point2d_idx;
    }
  };
  std::vector<std::pair<KeypointKey, Eigen::Vector2d>> kpt_sorted;
  kpt_sorted.reserve(observations.size());
  for (const TriangulationObservation& obs : observations) {
    if (!obs.keypoint_2d.allFinite()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "retriangulate: non-finite keypoint in observations");
    }
    kpt_sorted.emplace_back(
        KeypointKey{obs.image_id, obs.point2d_idx}, obs.keypoint_2d);
  }
  std::sort(kpt_sorted.begin(), kpt_sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  auto find_keypoint =
      [&kpt_sorted](std::uint32_t image_id,
                    std::int32_t point2d_idx) -> const Eigen::Vector2d* {
    const KeypointKey key{image_id, point2d_idx};
    auto it = std::lower_bound(
        kpt_sorted.begin(), kpt_sorted.end(), key,
        [](const auto& a, const KeypointKey& b) { return a.first < b; });
    if (it != kpt_sorted.end() && it->first.image_id == image_id &&
        it->first.point2d_idx == point2d_idx) {
      return &it->second;
    }
    return nullptr;
  };

  // --- 2. Triangulate each v2 point ---

  struct PointCandidate {
    std::uint64_t old_point3d_id = 0;
    std::array<double, 3> xyz{};
    std::array<double, 3> v2_xyz{};
    double parallax_deg = 0.0;
    double mean_residual_px = 0.0;
    TriangulationAcceptance geometric_acceptance =
        TriangulationAcceptance::kRejectedNonFinite;
    bool reprojection_acceptance = false;
    core::ReconPoint3D::TrackElement view1_track{};
    core::ReconPoint3D::TrackElement view2_track{};
    Eigen::Vector2d pixel1{};
    Eigen::Vector2d pixel2{};
    CameraView* view1_ptr = nullptr;
    CameraView* view2_ptr = nullptr;
  };

  std::vector<PointCandidate> candidates;
  candidates.reserve(v2.points3D.size());

  for (const core::ReconPoint3D& pt : v2.points3D) {
    PointCandidate cand;
    cand.old_point3d_id = pt.point3d_id;
    cand.v2_xyz = pt.xyz;

    // Need at least 2 track elements.
    if (pt.track.size() < 2) {
      cand.geometric_acceptance = TriangulationAcceptance::kRejectedNonFinite;
      candidates.push_back(std::move(cand));
      continue;
    }

    // Resolve keypoints for the first 2 track elements.
    const auto& t1 = pt.track[0];
    const auto& t2 = pt.track[1];
    const Eigen::Vector2d* k1 = find_keypoint(t1.image_id, t1.point2d_idx);
    const Eigen::Vector2d* k2 = find_keypoint(t2.image_id, t2.point2d_idx);

    if (k1 == nullptr || k2 == nullptr) {
      cand.geometric_acceptance = TriangulationAcceptance::kRejectedNonFinite;
      candidates.push_back(std::move(cand));
      continue;
    }

    CameraView* v1 = find_view(t1.image_id);
    CameraView* v2v = find_view(t2.image_id);

    if (v1 == nullptr || v2v == nullptr) {
      cand.geometric_acceptance = TriangulationAcceptance::kRejectedNonFinite;
      candidates.push_back(std::move(cand));
      continue;
    }

    cand.view1_track = t1;
    cand.view2_track = t2;
    cand.pixel1 = *k1;
    cand.pixel2 = *k2;
    cand.view1_ptr = v1;
    cand.view2_ptr = v2v;

    // Triangulate from 2 rays.
    TriangulationCandidate tc = TriangulateTwoRays(
        *v1, *k1, *v2v, *k2, options.min_parallax_deg);

    cand.geometric_acceptance = tc.acceptance;
    cand.xyz = tc.xyz;
    cand.parallax_deg = tc.parallax_deg;

    candidates.push_back(std::move(cand));
  }

  // --- 3. Compute all reprojection residuals for geometrically accepted
  //     candidates, then compute the global D5 threshold. ---

  // First pass: compute per-candidate residuals for geometrically accepted.
  std::vector<double> all_residuals;
  for (PointCandidate& cand : candidates) {
    if (cand.geometric_acceptance != TriangulationAcceptance::kAccepted) {
      cand.reprojection_acceptance = false;
      continue;
    }

    // Compute mean reprojection error across ALL track observations.
    // Resolve the full track from v2.
    const core::ReconPoint3D* v2pt = nullptr;
    for (const core::ReconPoint3D& pt : v2.points3D) {
      if (pt.point3d_id == cand.old_point3d_id) {
        v2pt = &pt;
        break;
      }
    }
    if (v2pt == nullptr) {
      cand.reprojection_acceptance = false;
      continue;
    }

    std::vector<Eigen::Vector3d> cam_positions;
    std::vector<Eigen::Matrix3d> cam_rotations;
    std::vector<CameraModel> cam_models;
    std::vector<Eigen::Vector2d> kpts;

    for (const core::ReconPoint3D::TrackElement& te : v2pt->track) {
      CameraView* view = find_view(te.image_id);
      const Eigen::Vector2d* kpt = find_keypoint(te.image_id, te.point2d_idx);
      if (view == nullptr || kpt == nullptr) continue;

      cam_positions.push_back(view->world_from_camera.translation());
      cam_rotations.push_back(
          view->world_from_camera.rotation().ToRotationMatrix());
      cam_models.push_back(view->model);
      kpts.push_back(*kpt);
    }

    if (cam_positions.size() < 2) {
      cand.reprojection_acceptance = false;
      continue;
    }

    cand.mean_residual_px = ComputePointReprojectionError(
        cand.xyz, cam_positions, cam_rotations, cam_models, kpts);

    if (!std::isfinite(cand.mean_residual_px)) {
      cand.reprojection_acceptance = false;
      continue;
    }

    all_residuals.push_back(cand.mean_residual_px);
  }

  // Compute global D5 threshold from all residuals of accepted candidates.
  double global_threshold = 2.0;  // absolute floor (D5)
  if (!all_residuals.empty()) {
    std::vector<double> sorted_residuals = all_residuals;
    std::sort(sorted_residuals.begin(), sorted_residuals.end());
    const std::size_t n = sorted_residuals.size();
    const double median =
        (n % 2 == 0) ? 0.5 * (sorted_residuals[n / 2 - 1] +
                               sorted_residuals[n / 2])
                      : sorted_residuals[n / 2];
    const double d5_threshold = 3.0 * median;
    if (d5_threshold > global_threshold) {
      global_threshold = d5_threshold;
    }
  }

  // Second pass: accept/reject based on D5 threshold.
  for (PointCandidate& cand : candidates) {
    if (cand.geometric_acceptance != TriangulationAcceptance::kAccepted) {
      cand.reprojection_acceptance = false;
      continue;
    }
    cand.reprojection_acceptance =
        (cand.mean_residual_px <= global_threshold);
    if (!cand.reprojection_acceptance) {
      cand.geometric_acceptance = TriangulationAcceptance::kRejectedHighResidual;
    }
  }

  // --- 4. Classify lineage and build v3 ---

  RetriangulationResult result;
  core::Reconstruction& v3 = result.reconstruction;

  // Copy v2 verbatim as baseline (cameras, images, scene metadata).
  v3 = v2;
  v3.reconstruction_id = core::FormatUuid(core::GenerateUuid());
  v3.status = "succeeded";
  v3.created_at_ns = 0;  // deterministic, no wall-clock (§11.3)

  // Provenance (§4.11, mirror ApplyOptimizedTrajectory pattern).
  core::ReconstructionProvenance prov;
  prov.backend.name = "spatial_retriangulator";
  prov.backend.version = "1.0.0";
  prov.backend.adapter_version = "1.0.0";
  // input_artifact_hashes: inherit + append v2 reconstruction id.
  std::vector<std::string> hashes = v2.provenance.input_artifact_hashes;
  if (!v2.reconstruction_id.empty()) {
    if (std::find(hashes.begin(), hashes.end(), v2.reconstruction_id) ==
        hashes.end()) {
      hashes.push_back(v2.reconstruction_id);
    }
  }
  std::sort(hashes.begin(), hashes.end());
  prov.input_artifact_hashes = std::move(hashes);
  v3.provenance = prov;

  // Replace points3D with re-triangulated geometry.
  v3.points3D.clear();
  std::uint64_t next_point_id = 10001u;  // fresh IDs, never reuse v2 IDs

  for (PointCandidate& cand : candidates) {
    PointLineageEntry entry;
    entry.old_point3d_id = cand.old_point3d_id;

    bool accepted = (cand.geometric_acceptance ==
                     TriangulationAcceptance::kAccepted) &&
                    cand.reprojection_acceptance;

    if (!accepted) {
      // REJECTED: no point emitted (§4.10 fail-closed).
      entry.lineage = PointLineage::kRejected;
      result.lineage.rejected_count++;
      result.point_lineage.push_back(std::move(entry));
      continue;
    }

    // Compute displacement from v2.
    const double dx = cand.xyz[0] - cand.v2_xyz[0];
    const double dy = cand.xyz[1] - cand.v2_xyz[1];
    const double dz = cand.xyz[2] - cand.v2_xyz[2];
    const double displacement = std::sqrt(dx * dx + dy * dy + dz * dz);

    // Find the original v2 point to copy the track.
    const core::ReconPoint3D* v2pt = nullptr;
    for (const core::ReconPoint3D& pt : v2.points3D) {
      if (pt.point3d_id == cand.old_point3d_id) {
        v2pt = &pt;
        break;
      }
    }

    if (displacement < options.stability_threshold_m) {
      // PRESERVED: geometry nearly unchanged, keep original id.
      entry.lineage = PointLineage::kPreserved;
      entry.new_point3d_id = cand.old_point3d_id;
      result.lineage.preserved_count++;
    } else {
      // REPLACED: geometry changed significantly, new id.
      entry.lineage = PointLineage::kReplaced;
      entry.new_point3d_id = next_point_id++;
      result.lineage.replaced_count++;
    }

    // Build v3 point.
    core::ReconPoint3D new_pt;
    new_pt.point3d_id = entry.new_point3d_id;
    new_pt.xyz = cand.xyz;
    if (v2pt != nullptr) {
      new_pt.color = v2pt->color;
      new_pt.track = v2pt->track;
    }
    new_pt.error = cand.mean_residual_px;
    v3.points3D.push_back(new_pt);

    result.point_lineage.push_back(std::move(entry));
  }

  return result;
}

}  // namespace spatial::core::geometry
