#include "engine/pipeline/feature_extraction.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/uuid.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactWriteResult;
using spatial::core::DeriveFeatureSetId;
using spatial::core::ErrorCode;
using spatial::core::FeatureSetRow;
using spatial::core::FormatUuid;
using spatial::core::ParseUuid;
using spatial::core::ProjectError;
using nlohmann::json;

json KeypointJson(const FeaturePoint& p) {
  return json{{"x", p.x}, {"y", p.y}, {"size", p.size},
              {"angle", p.angle}, {"response", p.response}};
}

// Builds the canonical FeatureArtifact payload + manifest and writes it into
// the CAS. Shared by the scene-agnostic payload writer and the scene-aware
// feature_sets writer so the produced bytes are byte-identical (one canonical
// producer, RFC-0007 §6, AC-8).
ArtifactWriteResult WritePayload(spatial::core::ArtifactStore& store,
                                 const WriteFeatureArtifactInput& input) {
  // Producer guarantee (RFC-0007 §2): count == keypoints.length ==
  // descriptors.length. Not expressible in JSON Schema; enforced here.
  if (input.keypoints.size() != input.descriptors.size()) {
    throw ProjectError(ErrorCode::kValidationDomain,
                       "feature extraction: keypoints/descriptors length "
                       "mismatch");
  }

  // Canonical payload: nlohmann objects serialize with sorted keys, so the
  // bytes (and hence the CAS hash) are a pure function of the inputs.
  json keypoints = json::array();
  for (const auto& p : input.keypoints) {
    keypoints.push_back(KeypointJson(p));
  }
  json descriptors = json::array();
  for (const auto& row : input.descriptors) {
    json jrow = json::array();
    for (const double v : row) {
      jrow.push_back(v);
    }
    descriptors.push_back(std::move(jrow));
  }
  const json payload_json = {
      {"detector", input.detector},
      {"descriptor_type", input.descriptor_type},
      {"count", static_cast<std::int64_t>(input.keypoints.size())},
      {"keypoints", keypoints},
      {"descriptors", descriptors},
      {"schema_version", 1},
  };
  const std::string payload_text = payload_json.dump();
  const std::vector<std::uint8_t> payload(payload_text.begin(),
                                          payload_text.end());

  ArtifactManifest manifest;
  manifest.type = "feature";
  manifest.schema_version = 1;
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.input_artifact_hashes = {input.input_content_hash};
  manifest.configuration_hash = input.configuration_hash;
  manifest.coordinate_frame = "image";
  manifest.unit = "pixels";
  manifest.mime_type = "application/json";

  return store.Put(payload, manifest);
}

}  // namespace

FeatureExtractionResult WriteFeatureArtifactPayload(
    spatial::core::ArtifactStore& store, const WriteFeatureArtifactInput& input) {
  const auto written = WritePayload(store, input);

  FeatureExtractionResult result;
  result.artifact_uuid = written.artifact_uuid;
  result.content_hash = written.content_hash;
  result.deduplicated = written.deduplicated;
  return result;
}

FeatureExtractionResult WriteFeatureArtifact(
    spatial::core::ArtifactStore& store, spatial::core::MetadataDb& db,
    const WriteFeatureArtifactInput& input) {
  const auto written = WritePayload(store, input);

  const auto feature_set_id =
      DeriveFeatureSetId(input.frame_id, written.content_hash);

  // Idempotent re-execution (RFC-0007 §6, AC-8): the deterministic id means a
  // prior run already recorded this (frame, content) tuple; re-inserting would
  // violate the PK, so return the existing record instead.
  for (const auto& existing : db.FindFeatureSetsByFrame(input.frame_id)) {
    if (existing.feature_set_id == feature_set_id) {
      FeatureExtractionResult out;
      out.feature_set.feature_set_id = existing.feature_set_id;
      out.feature_set.frame_id = existing.frame_id;
      out.feature_set.detector = existing.detector;
      out.feature_set.descriptor_type = existing.descriptor_type;
      out.feature_set.count = existing.count;
      out.feature_set.artifact_ref = ParseUuid(existing.artifact_ref);
      out.artifact_uuid = out.feature_set.artifact_ref;
      out.content_hash = written.content_hash;
      out.deduplicated = true;
      return out;
    }
  }

  FeatureSetRow row;
  row.feature_set_id = feature_set_id;
  row.frame_id = input.frame_id;
  row.detector = input.detector;
  row.descriptor_type = input.descriptor_type;
  row.count = static_cast<std::int64_t>(input.keypoints.size());
  row.artifact_ref = FormatUuid(written.artifact_uuid);
  db.InsertFeatureSet(row);

  FeatureExtractionResult result;
  result.feature_set.feature_set_id = feature_set_id;
  result.feature_set.frame_id = input.frame_id;
  result.feature_set.detector = input.detector;
  result.feature_set.descriptor_type = input.descriptor_type;
  result.feature_set.count = row.count;
  result.feature_set.artifact_ref = written.artifact_uuid;
  result.artifact_uuid = written.artifact_uuid;
  result.content_hash = written.content_hash;
  result.deduplicated = written.deduplicated;
  return result;
}

void RegisterFeatureExtraction(PipelineRegistry& registry) {
  PipelineDefinition def;
  def.id = kFeatureExtractionPipelineId;
  def.name = "Feature Extraction";
  def.version = "0.1.0";
  def.git_commit = kEngineGitCommit;
  def.config_schema_json = "{}";
  def.stages = {
      {"feature_extract", "feature_extraction", "feature_extract",
       {"image"}, {"feature"}},
  };
  registry.Register(std::move(def));
}

}  // namespace spatial::engine
