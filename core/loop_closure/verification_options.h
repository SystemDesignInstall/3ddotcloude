#pragma once

// P3-impl-7b: configuration for geometric loop-closure verification.
//
// All acceptance thresholds are CONFIGURABLE here (P3-impl-7b §8, §10) — never
// hard-coded in Core. They are pure data: identical options produce identical
// verification results (D-DI-02). Defaults follow the established D-LC-04 /
// D-LC-09 semantics and mirror the 6c LoopClosurePipelineOptions fields where
// they overlap (so 7b reuses the same contract and thresholds per §16).
//
// A single feature_match_score SHALL NEVER cause acceptance (§10): acceptance
// requires the geometric evidence gates below (correspondences, model, inliers,
// inlier-ratio, residual, confidence) to all hold.

#include <cstddef>
#include <cstdint>

namespace spatial::core {

struct GeometricVerificationOptions {
  // Minimum number of reconstructed correspondences required even to attempt
  // geometric estimation. Below this -> REJECTED (insufficient correspondences).
  std::size_t min_correspondences = 20;

  // D-LC-04 absolute floor on geometrically consistent inliers.
  std::size_t min_inliers = 8;

  // D-LC-04 floor on inlier fraction (inliers / correspondences).
  double min_inlier_ratio = 0.7;

  // Maximum acceptable geometric (mean Sampson) residual in pixels. Above this
  // -> REJECTED (geometric error too high).
  double max_residual = 4.0;

  // Robust-estimation (RANSAC) parameters. Iteration limit is bounded
  // (deterministic); confidence drives the stopping criterion.
  std::size_t max_ransac_iterations = 256;
  double ransac_confidence = 0.999;

  // Sampson-distance (pixels) inlier cutoff for the robust estimator.
  double ransac_inlier_threshold = 1.0;

  // D-LC-06: temporal separation below this -> near-duplicate, reject.
  std::int64_t minimum_temporal_separation_ns = 0;

  // D-LC-09: minimum combined confidence for acceptance.
  double verify_min_confidence = 0.6;
};

}  // namespace spatial::core
