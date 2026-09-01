#pragma once

// P3-impl-7b: classical, deterministic, UNCALIBRATED fundamental-matrix + RANSAC
// geometric verifier (adapters/visual_geometry/, readiness §7, §14).
//
// This is the first concrete implementation of the backend-independent
// GeometricVerifier contract. It is pure, deterministic, and uses only standard
// C++ + Eigen (no OpenCV / COLMAP / GTSAM). It exists to prove the geometric
// verification seam end-to-end.
//
// Geometry posture (P3-impl-7b §7 / §11 / G2):
//   - The FeatureArtifact contract carries NO camera intrinsics, so this
//     adapter is UNCALIBRATED and estimates a FUNDAMENTAL matrix -> it verifies
//     epipolar/geometric CONSISTENCY only. It NEVER emits a metric relative
//     pose (has_relative_pose is always false here), because doing so would
//     fabricate metric scale that the evidence does not provide.
//   - A calibrated (ESSENTIAL -> metric pose) adapter is a documented extension
//     point behind the same GeometricVerifier contract for a later stage.
//
// Determinism (D-DI-02): the robust estimator uses a fixed, recorded seed (not
// wall-clock), so identical inputs + options yield identical results.

#include <cstdint>

#include "core/loop_closure/geometric_verifier.h"

namespace spatial::adapters::visual_geometry {

// Uncalibrated fundamental-matrix + RANSAC geometric verifier. Produces an
// accepted/rejected canonical LoopClosure from image-space correspondences.
class FundamentalGeometricVerifier
    : public spatial::core::GeometricVerifier {
 public:
  spatial::core::GeometricVerificationResult Verify(
      const spatial::core::LoopClosureVerificationInput& input,
      const spatial::core::GeometricVerificationOptions& options)
      const override;
};

}  // namespace spatial::adapters::visual_geometry
