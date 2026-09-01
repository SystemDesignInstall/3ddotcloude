#pragma once

// P3-impl-7b: reconstruction of image-space point correspondences from two
// frames' FeatureArtifacts via the descriptor matcher (readiness §4, §5, §6).
//
// A LoopClosureCandidate carries only a scalar score; geometric verification
// needs (x1,y1) <-> (x2,y2) pairs. We obtain them WITHOUT changing the
// candidate contract: the source/target FeatureArtifact keypoints+descriptors
// are the input, and the matcher's additive MatchCorrespondences() seam
// supplies the index pairs. The coordinates are then read directly from each
// frame's keypoints[].
//
// This is pure, deterministic core logic (D-DI-02): identical features +
// matcher + options yield identical correspondences. The correspondences are
// an ephemeral verification input, never a canonical artifact.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/errors/project_error.h"
#include "core/loop_closure/feature_matcher.h"

namespace spatial::core {

// Reconstructs and validates the point correspondences between `source` (newer)
// and `target` (older) frames' features, using the matcher's correspondence
// seam.
//
// Validation performed at source or target index i:
//   - the matcher's index is within [0, descriptors.size()) and
//     keypoints.size() (structural; a violation is a malformed matcher/adapter
//     result and throws ProjectError(kValidationDomain) — P3-impl-7b §13-B);
//   - the referenced keypoint coordinates are finite (non-finite points are
//     dropped, never fed to the estimator — §6 / §12).
//
// Frame-level structural errors (empty descriptors, mismatch between keypoints
// and descriptors lengths) also throw ProjectError(kValidationDomain).
inline std::vector<FeatureCorrespondence> ReconstructCorrespondences(
    const MatchingFrameDescriptors& source,
    const MatchingFrameDescriptors& target,
    const LoopClosureFeatureMatcher& matcher) {
  if (source.descriptors.empty() || target.descriptors.empty() ||
      source.keypoints.size() != source.descriptors.size() ||
      target.keypoints.size() != target.descriptors.size()) {
    throw ProjectError(
        ErrorCode::kValidationDomain,
        "loop closure: feature data invalid (empty or keypoint/descriptor "
        "count mismatch)");
  }

  std::vector<FeatureCorrespondence> out;
  std::vector<bool> seen_target(target.descriptors.size(), false);
  for (const FeatureCorrespondence& c :
       matcher.MatchCorrespondences(source, target)) {
    // Structural index bounds (malformed adapter output -> typed error).
    if (c.source_index >= source.descriptors.size() ||
        c.target_index >= target.descriptors.size()) {
      throw ProjectError(
          ErrorCode::kValidationDomain,
          "loop closure: matcher returned out-of-range correspondence index");
    }
    // Finite-value guard on the geometry we will actually use.
    if (!std::isfinite(source.keypoints[c.source_index].x) ||
        !std::isfinite(source.keypoints[c.source_index].y) ||
        !std::isfinite(target.keypoints[c.target_index].x) ||
        !std::isfinite(target.keypoints[c.target_index].y)) {
      continue;  // §6: drop non-finite points, never feed them to estimation
    }
    // Deduplicate (a target point claimed by more than one source point).
    if (seen_target[c.target_index]) continue;
    seen_target[c.target_index] = true;
    out.push_back(c);
  }
  return out;
}

}  // namespace spatial::core
