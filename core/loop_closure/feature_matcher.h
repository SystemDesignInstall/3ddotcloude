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

// One keypoint's image-space coordinates (feature.schema.json §2). A
// backend-independent image position; core keeps only what geometry needs.
// (The engine's FeaturePoint adds size/angle/response for detection; this
// minimal core form is what geometric verification consumes.)
struct FeatureKeypoint {
  double x = 0.0;
  double y = 0.0;
};

// One frame's set of keypoints + descriptors, the canonical input to a
// matcher and to geometric verification. Descriptor rows follow the
// FeatureArtifact contract (feature.schema.json):
//   descriptors[i] has dimensionality descriptor_dim, one row per keypoint i;
//   keypoints[i] (x, y) is the image-space position of keypoint i.
// The producer guarantee count == keypoints.length == descriptors.length
// (RFC-0007 §2) holds. Frame identity is carried as the canonical UUID string
// to match the LoopClosureCandidate frame fields (source/target are strings).
//
// `keypoints` is an additive field introduced by P3-impl-7b so that geometric
// verification can reconstruct image-space correspondences; the 7a matcher
// contract operates on descriptors and is unaffected.
struct MatchingFrameDescriptors {
  std::string frame_id;                 // canonical UUID string
  std::int64_t timestamp_ns = 0;        // frame timestamp (D-LC-06 separation)
  std::string descriptor_type;          // e.g. "mock_16"
  std::vector<FeatureKeypoint> keypoints;       // one per keypoint (additive, 7b)
  std::vector<std::vector<double>> descriptors;  // one row per keypoint
};

// A reconstructed point correspondence between two frames' FeatureArtifacts
// (P3-impl-7b §4). Ephemeral: an internal verification input, NOT a permanent
// canonical artifact and NEVER stored on LoopClosureCandidate. source_index /
// target_index index into the source/target MatchingFrameDescriptors' keypoints
// and descriptors arrays.
struct FeatureCorrespondence {
  std::uint32_t source_index = 0;       // index into source keypoints/descriptors
  std::uint32_t target_index = 0;       // index into target keypoints/descriptors
  double descriptor_distance = 0.0;     // matcher distance for this pair
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

  // Reconstructs the point correspondences (source_index <-> target_index)
  // implied by this matcher's descriptor matching (P3-impl-7b §4 / G1). This is
  // the additive seam through which geometric verification obtains image-space
  // point pairs from the descriptor matcher. Deterministic for identical inputs
  // (D-DI-02) and consistent with MatchScore().
  //
  // The base default returns an empty set, meaning "this matcher cannot supply
  // correspondences" — a verification consumer FAILS CLOSED (insufficient
  // correspondences -> REJECTED) rather than fabricating links. Concrete
  // matchers SHOULD override to expose their real nearest-neighbour pairs.
  virtual std::vector<FeatureCorrespondence> MatchCorrespondences(
      const MatchingFrameDescriptors& a,
      const MatchingFrameDescriptors& b) const {
    (void)a;
    (void)b;
    return {};
  }
};

}  // namespace spatial::core
