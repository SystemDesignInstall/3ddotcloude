#pragma once

// Feature Extraction output (RFC-0007 §2-3, §6). WriteFeatureArtifactPayload
// turns a frame's keypoints/descriptors into the canonical FeatureArtifact
// payload (feature.schema.json, M0 JSON-array) and manifest, without touching
// the metadata DB; WriteFeatureArtifact additionally records the per-frame
// feature_sets row. The produced bytes are a pure function of the inputs, so
// identical runs dedupe in the CAS and replay from the ADR-020 task cache
// (AC-8). The scene-agnostic payload writer lets the mock worker produce the
// canonical artifact behind the worker boundary (ADR-038); the feature_sets
// row is recorded at the session/CLI layer (RFC-0007 §8).

#include <cstdint>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/scene/feature/feature_set.h"
#include "core/storage/metadata_db.h"
#include "engine/pipeline/pipeline_registry.h"

namespace spatial::engine {

// One keypoint in image-space pixel coordinates (feature.schema.json §2).
struct FeaturePoint {
  double x = 0.0;
  double y = 0.0;
  double size = 0.0;
  double angle = 0.0;
  double response = 0.0;
};

struct WriteFeatureArtifactInput {
  spatial::core::Uuid frame_id{};
  std::string detector;          // canonical id, e.g. "mock" (RFC-0007 §2)
  std::string descriptor_type;   // e.g. "mock_16" (16-dim float rows)
  std::vector<FeaturePoint> keypoints;
  std::vector<std::vector<double>> descriptors;  // one row per keypoint
  std::string input_content_hash;  // image bytes hash (manifest input ref)
  // SHA-256 of the effective stage configuration that produced the payload
  // (manifest configuration_hash, artifact-manifest.schema.json). Callers
  // supply it; the payload writer is scene- and config-agnostic (ADR-038).
  // For the worker path it is Sha256Hex(request.config_json), the ADR-020
  // cache-key config hash; the session/CLI layer copies the worker's value.
  std::string configuration_hash;
};

struct FeatureExtractionResult {
  spatial::core::FeatureSet feature_set;
  spatial::core::Uuid artifact_uuid{};
  std::string content_hash;
  bool deduplicated = false;
};

// Writes ONLY the FeatureArtifact payload + manifest into `store` (type
// "feature", mime application/json, coordinate_frame "image", unit "pixels").
// The payload conforms to feature.schema.json; the producer guarantee
// count == keypoints.length == descriptors.length is enforced here (throws
// ProjectError kValidationDomain otherwise). Scene-agnostic (ADR-038): no
// frame/scene knowledge is required, so a worker can produce the canonical
// artifact without touching the metadata DB. The feature_sets row is recorded
// by the scene-aware layer (CLI/session, RFC-0007 §8) via WriteFeatureArtifact.
// The produced bytes are a pure function of the inputs, so identical runs
// dedupe in the CAS and replay from the ADR-020 task cache (AC-8).
FeatureExtractionResult WriteFeatureArtifactPayload(
    spatial::core::ArtifactStore& store, const WriteFeatureArtifactInput& input);

// Writes the FeatureArtifact payload + manifest via WriteFeatureArtifactPayload
// and records the per-frame feature_sets row via `db`. The feature_set_id is
// deterministic (DeriveFeatureSetId), so re-execution on identical inputs is
// idempotent (RFC-0007 §6, AC-8).
FeatureExtractionResult WriteFeatureArtifact(
    spatial::core::ArtifactStore& store, spatial::core::MetadataDb& db,
    const WriteFeatureArtifactInput& input);

// The single-stage Feature Extraction pipeline (RFC-0007 §6):
// feature_extract (capability "feature_extraction") {image} -> {feature}.
inline constexpr const char* kFeatureExtractionPipelineId =
    "feature_extraction";

// Registers the single-stage Feature Extraction pipeline (id
// kFeatureExtractionPipelineId) in `registry`. Safe to call on multiple
// registries; registration is additive.
void RegisterFeatureExtraction(PipelineRegistry& registry);

}  // namespace spatial::engine
