#include "engine/pipeline/loop_closure_detection.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_manifest.h"
#include "core/errors/project_error.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/pipeline_registry.h"
#include "engine_build_info.h"

namespace spatial::engine {

using spatial::core::ArtifactManifest;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::LoopClosureCandidate;
using spatial::core::LoopClosureCandidateRow;
using spatial::core::MatchingFrameDescriptors;
using spatial::core::ParseUuid;
using spatial::core::Uuid;
using nlohmann::json;

namespace {

json CandidateJson(const LoopClosureCandidate& c) {
  return json{{"candidate_id", c.candidate_id},
              {"trajectory_id", c.trajectory_id},
              {"source_frame_id", c.source_frame_id},
              {"target_frame_id", c.target_frame_id},
              {"feature_match_score", c.feature_match_score},
              {"matcher", c.matcher},
              {"created_at_ns", c.created_at_ns}};
}

}  // namespace

LoopClosureDetectionResult DetectLoopClosureCandidates(
    spatial::core::ArtifactStore& store, spatial::core::MetadataDb& db,
    const std::string& trajectory_id,
    const std::vector<LoopClosureFrameInput>& frames,
    const spatial::core::LoopClosureFeatureMatcher& matcher,
    const spatial::core::LoopClosureDetectionOptions& options,
    const std::string& configuration_hash) {
  LoopClosureDetectionResult result;
  result.frames_input = frames.size();
  result.matcher = matcher.MatcherId();

  // Resolve each frame's FeatureArtifact into canonical descriptor sets, in
  // input order (ascending timestamp contract). Unresolvable frames are
  // skipped — they contribute no descriptors and no candidates.
  std::vector<MatchingFrameDescriptors> descriptor_sets;
  descriptor_sets.reserve(frames.size());
  std::vector<std::string> input_hashes;  // provenance: source FeatureArtifacts
  for (const auto& f : frames) {
    const auto loaded = spatial::core::LoadFeatureDescriptors(
        store, f.feature_artifact_uuid, FormatUuid(f.frame_id),
        f.timestamp_ns);
    if (!loaded) continue;
    const auto manifest = store.ReadManifest(f.feature_artifact_uuid);
    if (manifest) input_hashes.push_back(manifest->content_hash);
    descriptor_sets.push_back(std::move(*loaded));
  }
  result.artifacts_resolved = descriptor_sets.size();

  result.candidates = spatial::core::GenerateLoopClosureCandidates(
      descriptor_sets, matcher, options, trajectory_id);

  // Persist each candidate to the existing loop_closure_candidates table.
  const Uuid traj = ParseUuid(trajectory_id);
  for (const auto& c : result.candidates) {
    LoopClosureCandidateRow row;
    row.candidate_id = ParseUuid(c.candidate_id);
    row.trajectory_id = traj;
    row.source_frame_id = ParseUuid(c.source_frame_id);
    row.target_frame_id = ParseUuid(c.target_frame_id);
    row.feature_match_score = c.feature_match_score;
    row.matcher = c.matcher;
    row.created_at_ns = c.created_at_ns;
    db.AddLoopClosureCandidate(row);
  }

  // Canonical loop-closure CAS payload (loop-closure.schema.json): candidates
  // populated, closures empty (verification is 7b, out of scope here).
  json candidates = json::array();
  for (const auto& c : result.candidates) {
    candidates.push_back(CandidateJson(c));
  }
  const json payload_json = {
      {"schema_version", 1},
      {"candidates", candidates},
      {"closures", json::array()},
  };
  const std::string payload_text = payload_json.dump();
  const std::vector<std::uint8_t> payload(payload_text.begin(),
                                          payload_text.end());

  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "loop_closure";
  manifest.schema_version = 1;
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.input_artifact_hashes = input_hashes;
  manifest.configuration_hash = configuration_hash;
  manifest.coordinate_frame = "image";
  manifest.unit = "matches";
  manifest.mime_type = "application/json";

  const auto written = store.Put(payload, manifest);
  result.payload_content_hash = written.content_hash;
  result.payload_artifact_uuid = written.artifact_uuid;

  return result;
}

void RegisterLoopClosureDetection(PipelineRegistry& registry) {
  PipelineDefinition def;
  def.id = kLoopClosureDetectionPipelineId;
  def.name = "Visual Loop Closure Candidate Detection";
  def.version = "0.1.0";
  def.git_commit = kEngineGitCommit;
  def.config_schema_json = "{}";
  def.stages = {
      {"loop_closure_detect", "loop_closure", "loop_closure_detect",
       {"feature", "frame"}, {"loop_closure_candidate"}},
  };
  registry.Register(std::move(def));
}

}  // namespace spatial::engine
