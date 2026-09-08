#pragma once

// Canonical Bundle Adjustment SEAM (P3-impl-8c, P1/P2/P3; docs/architecture/
// P3-impl-8c-bundle-adjustment-readiness.md). The single engine-facing
// interface for running photogrammetric bundle adjustment (fixed intrinsics,
// D3). The backend — COLMAP's `bundle_adjuster` subprocess, reached ONLY
// behind this seam — is never linked and never appears in engine/pipeline:
// this header is header-only, canonical, and carries no adapter type, no DB,
// and no execution plan.
//
// Pose convention: T_reconstruction_camera (world-from-camera), quaternion
// (x, y, z, w) scalar-last (ADR-007). Intrinsics (fx/fy/cx/cy/distortion) are
// FIXED constants for 8c (D3): the seam input carries them verbatim in
// `source.cameras` and the output must reproduce them byte-identically.
//
// The seam consumes the SAME deterministic residual definition as 8a/8b
// (reprojection.h): r_ij = ||keypoint_ij - proj_i(p_R_j)|| px. The D5 metrics
// and the acceptance gate are the frozen 8a predicates; no new reprojection
// math enters here.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/geometry/reprojection.h"
#include "core/reconstruction/reconstruction.h"

namespace spatial::core::geometry {

// Input for one bundle-adjustment run (P1). `source` is the v3 canonical
// Reconstruction (immutable read — revisions are never mutated). `observations`
// are the resolved 8a 2D->3D associations (keypoints from the canonical
// frame_id -> FeatureSet -> FeatureArtifact chain; never stored inline in the
// document). `random_seed` MUST be set: a bundle_adjustment plan without a
// pinned seed fails closed (D6/P9). `min_parallax_deg` is the passthrough
// parallax guard shared with the 8b re-triangulation contract.
struct BundleAdjustmentInput {
  core::Reconstruction source;      // v3, immutable read
  std::vector<ReprojectionObservation> observations;
  double min_parallax_deg = 2.0;    // passthrough guard (8b reuse)
  std::optional<std::string> random_seed;  // pinned (D6); nullopt fails closed
  int max_iterations = 100;
};

// Per-run optimizer telemetry (P1, mirrors OptimizationTrace D-OPT-02
// semantics). The rms/mean/count fields are the D5 metrics of the SAME
// observation set evaluated against the v3 and v4 geometries respectively.
struct BundleAdjustmentTrace {
  bool converged = false;
  std::int64_t iterations = 0;
  double rms_before_px = 0.0;
  double rms_after_px = 0.0;
  double mean_before_px = 0.0;
  double mean_after_px = 0.0;
  std::int64_t inlier_count_before = 0;
  std::int64_t outlier_count_before = 0;
  std::int64_t inlier_count_after = 0;
  std::int64_t outlier_count_after = 0;
  double threshold_px_before = 0.0;  // D5 max(3*median, 2px), v3
  double threshold_px_after = 0.0;   // D5 max(3*median, 2px), v4
};

// Result of one bundle-adjustment run through the seam (P1/P3).
struct BundleAdjustmentResult {
  core::Reconstruction reconstruction;  // v4 (fresh id, status="succeeded")
  BundleAdjustmentTrace trace;
};

// Backend-neutral bundle-adjustment seam. engine/ dispatches exclusively
// through this interface; each backend (currently COLMAP) supplies an
// implementation behind it. The result is used only when the acceptance gate
// passes (the engine orchestration stage enforces PassesReprojectionGate).
class ReconstructionOptimizer {
 public:
  virtual ~ReconstructionOptimizer() = default;
  virtual BundleAdjustmentResult optimize(const BundleAdjustmentInput&) = 0;
};

// Deterministic helper: the (point3d_id, xyz) list evaluated by
// EvaluateReprojection, in Reconstruction points3D order (D-CRM-08). Points
// that carry no observation are still reported (the evaluator tolerates and
// classifies them by track membership). Pure, ADR-020.
inline std::vector<std::pair<std::uint64_t, std::array<double, 3>>>
ReconstructionPoints(const core::Reconstruction& rec) {
  std::vector<std::pair<std::uint64_t, std::array<double, 3>>> points;
  points.reserve(rec.points3D.size());
  for (const core::ReconPoint3D& pt : rec.points3D) {
    points.emplace_back(pt.point3d_id, pt.xyz);
  }
  return points;
}

}  // namespace spatial::core::geometry