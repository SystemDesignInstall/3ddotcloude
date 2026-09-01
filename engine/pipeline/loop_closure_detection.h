#pragma once

// P3-impl-7a: the Visual Loop Closure Candidate-generation pipeline stage
// (capability "loop_closure") — candidate generation ONLY (AUTHORIZED Q2).
//
// Contract:
//   Frame/FeatureArtifact (one per input frame)
//        -> descriptor matching (via LoopClosureFeatureMatcher)
//        -> bounded/windowed candidate ranking + temporal exclusion
//        -> LoopClosureCandidate[]
//        -> metadata rows (existing loop_closure_candidates) + CAS payload
//           (loop-closure.schema.json) with provenance.
//
// This mirrors the relationship between the scene-agnostic payload writer and
// the scene-aware writer in feature_extraction: core/loop_closure holds the
// canonical, scene-agnostic generation logic; this engine layer resolves the
// frames' FeatureArtifacts, runs the injected matcher, and persists both the
// canonical metadata rows and the loop-closure CAS payload carrying the
// candidates + provenance.
//
// No geometric verification (7b), PoseGraph assembly, or GTSAM (7c) here.

#include <cstdint>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/loop_closure/loop_closure_candidate_gen.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "engine/pipeline/pipeline_registry.h"

namespace spatial::engine {

// A frame that participates in candidate search: its identity, timestamp, and
// the 16-byte UUID of its FeatureArtifact in the CAS.
struct LoopClosureFrameInput {
  spatial::core::Uuid frame_id{};
  std::int64_t timestamp_ns = 0;
  spatial::core::Uuid feature_artifact_uuid{};
};

// Result of one candidate-detection run: the canonical candidates plus the
// CAS artifact (loop-closure payload) and metadata evidence.
struct LoopClosureDetectionResult {
  std::vector<spatial::core::LoopClosureCandidate> candidates;
  std::string payload_content_hash;      // CAS content hash (loop-closure payload)
  spatial::core::Uuid payload_artifact_uuid{};
  std::string matcher;                   // canonical matcher id
  std::size_t frames_input = 0;          // number of frames given
  std::size_t artifacts_resolved = 0;    // frames with a resolvable FeatureArtifact
};

// Runs visual loop-closure candidate generation for a trajectory given its
// frames' FeatureArtifacts and a backend-independent matcher. Persists each
// surviving candidate to the existing loop_closure_candidates metadata table
// and writes the canonical loop-closure CAS payload (loop-closure.schema.json,
// candidates populated, closures empty) with an artifact manifest whose
// input_artifact_hashes reference the source FeatureArtifacts (provenance).
// Frames whose FeatureArtifact cannot be resolved are skipped (not an error).
LoopClosureDetectionResult DetectLoopClosureCandidates(
    spatial::core::ArtifactStore& store, spatial::core::MetadataDb& db,
    const std::string& trajectory_id,
    const std::vector<LoopClosureFrameInput>& frames,
    const spatial::core::LoopClosureFeatureMatcher& matcher,
    const spatial::core::LoopClosureDetectionOptions& options,
    const std::string& configuration_hash = "");

// The single-stage Visual Loop Closure Candidate-generation pipeline
// (capability "loop_closure" — the existing taxonomy name in
// worker-capabilities.schema.json):
//   loop_closure_detect (capability "loop_closure")
//     {feature, frame} -> {loop_closure_candidate}.
// Mirrors RegisterFeatureExtraction: a declarative registry entry; the stage
// producer is DetectLoopClosureCandidates above.
inline constexpr const char* kLoopClosureDetectionPipelineId =
    "loop_closure_detection";

// Registers the single-stage Visual Loop Closure Candidate-generation pipeline
// in `registry`. Safe to call on multiple registries; registration is additive.
void RegisterLoopClosureDetection(PipelineRegistry& registry);

}  // namespace spatial::engine
