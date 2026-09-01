#pragma once

// P3-impl-7a: the backend-independent visual loop-closure matching CONTRACT.
//
// Architecture boundary (Q1, mirrors the GTSAM adapter seam):
//  - Core defines the matching contract (MatchingFrameDescriptors + the
//    LoopClosureFeatureMatcher interface) and the canonical candidate
//    generation logic in loop_closure_candidate_gen.h. Core never names a
//    concrete matcher (OpenCV / FLANN / BFMatcher / a specific algorithm).
//  - A concrete matcher lives behind an adapter boundary (adapters/
//    visual_matching/) and implements LoopClosureFeatureMatcher.
//
// Normative decisions: D-LC-02/D-LC-03 (candidate), Q3 of P3-impl-7a (operate
// against the FeatureArtifact descriptor contract).

#include <cstdint>
#include <string>
#include <vector>

namespace spatial::core {

// One frame's set of descriptors, the canonical input to a matcher. Descriptor
// rows follow the FeatureArtifact contract (feature.schema.json):
//   descriptors[i] has dimensionality descriptor_dim, one row per keypoint.
// Frame identity is carried as the canonical UUID string to match the
// LoopClosureCandidate frame fields (source/target are strings).
struct MatchingFrameDescriptors {
  std::string frame_id;                 // canonical UUID string
  std::int64_t timestamp_ns = 0;        // frame timestamp (D-LC-06 separation)
  std::string descriptor_type;          // e.g. "mock_16"
  std::vector<std::vector<double>> descriptors;  // one row per keypoint
};

// Backend-independent matcher seam. The engine selects an implementation by
// capability and never by tool name (ADR-011 / ADR-034). Implementations MUST
// be deterministic for identical descriptor inputs (D-DI-02).
class LoopClosureFeatureMatcher {
 public:
  virtual ~LoopClosureFeatureMatcher() = default;

  // Raw match score in [0, inf) for the frame pair; backend-specific, higher
  // means a stronger match. Used by the canonical generator to rank and
  // threshold candidates (D-LC-02). Deterministic for identical inputs.
  virtual double MatchScore(const MatchingFrameDescriptors& a,
                            const MatchingFrameDescriptors& b) const = 0;

  // Canonical matcher id recorded on every produced candidate (e.g. the
  // loop-closure.schema.json `matcher` field). E.g. "visual_l2_nearest".
  virtual std::string MatcherId() const = 0;
};

}  // namespace spatial::core
