#pragma once

// P3-impl-8c: GTSAM Levenberg-Marquardt Bundle Adjustment adapter.
//
// Implements the canonical core::geometry::ReconstructionOptimizer seam for the
// v3 -> v4 revision step with FIXED intrinsics (D3). This header is the sole
// public surface of the adapter and carries NO GTSAM type: engine/pipeline
// reaches the backend only through the seam, so no GTSAM type leaks past the
// boundary (P2). The .cpp is the second GTSAM compile unit in the platform.
//
// Model coverage — fail closed, never approximated (§4.9 doctrine): every
// camera in the source must be a first-class model that the GTSAM Cal3DS2
// calibration reproduces EXACTLY: "pinhole" (Cal3DS2's zero-distortion
// specialization) or "opencv"/"opencv_radial" WITHOUT a k3 coefficient
// (k1,k2,p1,p2 verbatim). Any other combination (fisheye, fov, opencv with a
// non-zero k3) throws a typed validation error before any computation; the
// COLMAP backend remains the production path for those models behind the SAME
// seam.
//
// Determinism (D6): single-threaded LM over a deterministically ordered factor
// graph and deterministic prior anchoring; the pinned random_seed is validated
// (fail-closed when absent) and recorded, and the numeric result is a pure
// function of (source, observations, options, seed).
//
// Pose convention: T_reconstruction_camera (world-from-camera), quaternion
// (x, y, z, w) scalar-last (ADR-007) — the same convention as the seam's
// source document.

#include <string>

#include "core/geometry/reconstruction_optimizer.h"

namespace spatial::adapters::gtsam {

// Adapter configuration (D6-relevant; hashed into the v4 configuration_hash).
struct BundleAdjustmentOptions {
  // The robust loss family applied to the reprojection residuals. Only
  // "huber" is implemented today; the loss is a soft-inlier weighting and
  // never changes the frozen D5 metric path (reprojection.h classifies
  // inliers by threshold independent of the optimizer's loss).
  std::string robust_loss = "huber";
  // Robust-loss scale in pixels; 0.0 => the D5 threshold of the v3 evaluation
  // (max(3*median, 2) px), mirroring the COLMAP adapter's default.
  double robust_loss_scale_px = 0.0;
};

// Seam implementation backed by a real GTSAM batch Levenberg-Marquardt solve
// (GenericProjectionFactor + Cal3DS2, Huber robust loss, pose/point refinement
// in the reconstruction frame). The result is used only when the engine's
// frozen D5 acceptance gate (PassesReprojectionGate, strict 0.9) passes.
class GtsamBundleAdjustmentOptimizer final
    : public spatial::core::geometry::ReconstructionOptimizer {
 public:
  explicit GtsamBundleAdjustmentOptimizer(BundleAdjustmentOptions options = {});

  spatial::core::geometry::BundleAdjustmentResult optimize(
      const spatial::core::geometry::BundleAdjustmentInput& input) override;

 private:
  BundleAdjustmentOptions options_;
};

}  // namespace spatial::adapters::gtsam