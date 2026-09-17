#pragma once

// P3.1 Step 2: classical, deterministic, CALIBRATED essential-matrix + RANSAC
// geometric verifier (adapters/visual_geometry/).
//
// This is the second concrete implementation of the backend-independent
// GeometricVerifier contract (core/loop_closure/geometric_verifier.h): the
// calibrated (ESSENTIAL -> unit relative pose) adapter that
// fundamental_verifier.h documents as the extension point behind the same
// contract. FundamentalGeometricVerifier is untouched.
//
// Estimation vs scale-resolution separation (P3.1 Step 1, binding):
//   - This provider estimates R_source_target + t_hat (UNIT direction in the
//     source camera frame C_s) from calibrated observations ONLY.
//   - It NEVER emits metres: on success has_relative_pose stays false,
//     relative_position_xyz stays zero, and the estimate travels ONLY in
//     GeometricVerificationResult.unit_relative_pose.
//   - Metric scale is resolved downstream by the FROZEN
//     ResolveMetricTranslationFromUnitDirection (core/trajectory/
//     pose_graph_helpers.h), never here.
//   - No GTSAM anywhere on this path (std + Eigen only, same boundary as the
//     fundamental adapter). GTSAM EssentialMatrix/EssentialMatrixFactor may
//     only ever appear behind the optimizer seam as refinement, never as the
//     measurement source.
//
// Pipeline (all classical, Eigen-only):
//   1. ReconCamera -> CameraModel::FromReconCamera (fail-closed model
//      selection BEFORE any computation; throws typed CalibrationError /
//      ValidationError which propagate, never swallowed).
//   2. Pixels -> undistorted NORMALIZED coordinates
//      (UndistortUnit(PixelToNormalized(...))).
//   3. Hartley-normalized 8-point essential estimation + deterministic RANSAC
//      (fixed seed, D-DI-02), scored in PIXEL space by converting E -> F
//      (F = K_t^-T * E * K_s^-1) with the shared Sampson options, so residual
//      semantics match the fundamental path exactly.
//   4. Essential projection E = U diag(1,1,0) V^T, Hartley-Zisserman
//      decomposition into the 4 candidates (R1,+t) (R1,-t) (R2,+t) (R2,-t).
//   5. Cheirality vote: adapter-local linear triangulation on unit rays counts
//      points with positive depth in BOTH cameras per candidate; the unique
//      strict-majority winner is kept, ties fail closed ("ambiguous_cheirality").
//      (core/geometry/triangulation.h TriangulateTwoRays is NOT reused here:
//      it is world-frame CameraView plumbing with a 2-degree parallax gate for
//      v2->v3 re-triangulation, which would bias the relative-frame 4-way
//      vote; triangulation.h itself is untouched.)
//   6. Canonical conversion: the decomposition yields the epipolar-convention
//      pair (R_ess, t_ess) with X_t = R_ess X_s + t_ess. The platform's
//      canonical relative transform is T_source_target = T_ws^-1 * T_wt
//      (pose_graph_helpers.h:14-18, D-PG-04, RelativePoseBetween) — the
//      INVERSE map — so the provider exports
//         R_st = R_ess^T,  t_hat_st = -R_ess^T * t_hat_ess   (in C_s),
//      the form the frozen resolver scales and the GTSAM BetweenFactor
//      consumes verbatim. Exporting (R_ess, t_ess) directly would invert
//      every loop constraint; T2/T3 pin the convention against the frozen
//      RelativePoseBetween definition, not against backend names.
// Known limitation (fail-closed, documented): planar scenes underdetermine
// the 8-point essential fit; such inputs are expected to fail the residual /
// cheirality gates and reject rather than produce a pose.
//
// Determinism (D-DI-02): fixed RANSAC seed, no wall-clock, identical inputs +
// options yield identical results.

#include "core/loop_closure/geometric_verifier.h"

namespace spatial::adapters::visual_geometry {

// Calibrated essential-matrix + RANSAC geometric verifier. Produces an
// accepted/rejected canonical LoopClosure from image-space correspondences
// PLUS per-frame calibration; on success also yields the unit relative pose
// (R_source_target, t_hat in C_s) with has_relative_pose == false.
class EssentialGeometricVerifier : public spatial::core::GeometricVerifier {
 public:
  spatial::core::GeometricVerificationResult Verify(
      const spatial::core::LoopClosureVerificationInput& input,
      const spatial::core::GeometricVerificationOptions& options)
      const override;
};

}  // namespace spatial::adapters::visual_geometry
