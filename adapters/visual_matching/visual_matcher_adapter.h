#pragma once

// P3-impl-7a: classical visual matcher adapter (Q1 / Q3).
//
// This is the first concrete implementation of the backend-independent
// LoopClosureFeatureMatcher contract. It is a pure, deterministic, std-only
// classical matcher over FeatureArtifact descriptor rows — no OpenCV / FLANN /
// BFMatcher dependencies. It exists to prove the visual-loop-closure
// candidate path end-to-end from the existing FeatureArtifact contract.
//
// NOT a production visual descriptor:
//  - The score semantics are dimensionless "number of mutual nearest-neighbor
//    (reciprocal) matches below a distance threshold". For the deterministic
//    integration fixtures this cleanly separates a genuine revisit (shared
//    descriptors -> many matches) from unrelated frames (few matches).
//  - mock_16 / this geometry are for deterministic tests only (P3-impl-7a Q3).
//    A production descriptor / matcher is a separate later stage.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "core/loop_closure/feature_matcher.h"

namespace spatial::adapters::visual_matching {

// Deterministic L2 nearest-neighbour reciprocal-matching scorer.
class L2NearestMatcher : public spatial::core::LoopClosureFeatureMatcher {
 public:
  // Euclidean distance threshold below which two descriptors are a "match".
  // Eigenvalue-free and deterministic. Note: descriptor rows are floats in
  // [0,1] (mock_16), so an absolute distance threshold is meaningful for the
  // integration fixtures.
  explicit L2NearestMatcher(
      double distance_threshold = 0.05, bool mutual_only = true)
      : distance_threshold_(distance_threshold), mutual_only_(mutual_only) {}

  std::string MatcherId() const override { return "visual_l2_nearest"; }

  double MatchScore(const spatial::core::MatchingFrameDescriptors& a,
                    const spatial::core::MatchingFrameDescriptors& b)
      const override {
    if (a.descriptors.empty() || b.descriptors.empty()) return 0.0;
    const std::size_t dim = a.descriptors[0].size();
    if (dim == 0) return 0.0;

    // All rows must share the dimensionality of the first (feature.schema
    // producer guarantee implicit on write; guard defensively).
    for (const auto& row : a.descriptors)
      if (row.size() != dim) return 0.0;
    for (const auto& row : b.descriptors)
      if (row.size() != dim) return 0.0;

    std::vector<std::size_t> a_to_b(a.descriptors.size(), SIZE_MAX);
    std::vector<std::size_t> b_to_a(b.descriptors.size(), SIZE_MAX);
    for (std::size_t i = 0; i < a.descriptors.size(); ++i) {
      std::size_t best = 0;
      double best_d = std::numeric_limits<double>::infinity();
      for (std::size_t j = 0; j < b.descriptors.size(); ++j) {
        const double d = L2(a.descriptors[i], b.descriptors[j]);
        if (d < best_d) {
          best_d = d;
          best = j;
        }
      }
      if (best_d <= distance_threshold_) a_to_b[i] = best;
    }
    for (std::size_t j = 0; j < b.descriptors.size(); ++j) {
      std::size_t best = 0;
      double best_d = std::numeric_limits<double>::infinity();
      for (std::size_t i = 0; i < a.descriptors.size(); ++i) {
        const double d = L2(b.descriptors[j], a.descriptors[i]);
        if (d < best_d) {
          best_d = d;
          best = i;
        }
      }
      if (best_d <= distance_threshold_) b_to_a[j] = best;
    }

    std::size_t matches = 0;
    for (std::size_t i = 0; i < a_to_b.size(); ++i) {
      if (a_to_b[i] == SIZE_MAX) continue;
      if (mutual_only_ && b_to_a[a_to_b[i]] != i) continue;
      ++matches;
    }
    return static_cast<double>(matches);
  }

 private:
  static double L2(const std::vector<double>& x,
                   const std::vector<double>& y) {
    double s = 0.0;
    const std::size_t n = std::min(x.size(), y.size());
    for (std::size_t i = 0; i < n; ++i) {
      const double d = x[i] - y[i];
      s += d * d;
    }
    return std::sqrt(s);
  }

  double distance_threshold_;
  bool mutual_only_;
};

}  // namespace spatial::adapters::visual_matching
