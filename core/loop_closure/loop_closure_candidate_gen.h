#pragma once

// P3-impl-7a: canonical loop-closure CANDIDATE GENERATION over existing
// FeatureArtifact descriptors.
//
// Scope (AUTHORIZED, Q2/Q5): candidate generation ONLY. No geometric
// verification (7b), no PoseGraph assembly, no GTSAM (7c). Output is a vector
// of canonical LoopClosureCandidate records.
//
// Architecture boundary (Q1): the matching algorithm is injected via the
// backend-independent LoopClosureFeatureMatcher interface; core holds no
// concrete matcher. Candidate identity fields (candidate_id, created_at_ns)
// are instance identity (like D-PG-02/D-DI-01 for graphs) and are filled here;
// the deterministic fields (score, matcher, source/target) are a pure function
// of the inputs + options (D-DI-02).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/trajectory/loop_closure.h"

namespace spatial::core {

// Options for visual loop-closure candidate generation (P3-impl-7a Q5: bounded
// / windowed search; no large-scale global retrieval in 7a).
struct LoopClosureDetectionOptions {
  // D-LC-06 / D-LC-08: pairs closer than this are near-duplicates, not loops.
  std::int64_t minimum_temporal_separation_ns = 0;

  // Bounded lookback (Q5): a source frame i matches only against the
  // `search_window` immediately-older frames. 0 = full lookback (still paired
  // with older frames only, never future frames).
  std::size_t search_window = 0;

  // D-LC-02: raw score floor; pairs below are not candidates.
  double minimum_match_score = 0.0;

  // Keep at most this many (highest-scoring) candidates per source frame.
  // 0 = keep all above the score floor.
  std::size_t max_candidates_per_source = 0;
};

// Loads the canonical MatchingFrameDescriptors for one frame from the CAS by
// reading its FeatureArtifact payload (feature.schema.json) via the manifest's
// content hash. Returns nullopt when the artifact is absent or not a "feature".
// Throws ArtifactError if the payload parses but violates the producer
// guarantee (count == keypoints == descriptors lengths).
std::optional<MatchingFrameDescriptors> LoadFeatureDescriptors(
    ArtifactStore& store, const Uuid& artifact_uuid,
    const std::string& frame_id, std::int64_t timestamp_ns);

// Generates loop-closure candidates over an ordered list of frames' descriptor
// sets (ascending timestamp), source = newer frame, target = older frame
// (LoopClosureCandidate convention). Pairs outside the bounded search window,
// below the temporal separation minimum, or below the score floor are
// excluded; per-source top-k is applied when configured. `trajectory_id` is
// stamped on every candidate. Deterministic except for candidate_id /
// created_at_ns (instance identity).
std::vector<LoopClosureCandidate> GenerateLoopClosureCandidates(
    const std::vector<MatchingFrameDescriptors>& frames,
    const LoopClosureFeatureMatcher& matcher,
    const LoopClosureDetectionOptions& options,
    const std::string& trajectory_id = "");

}  // namespace spatial::core
