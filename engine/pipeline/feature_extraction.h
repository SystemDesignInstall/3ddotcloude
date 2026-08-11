#pragma once

// Feature Extraction output (RFC-0007 §2-3, §6). WriteFeatureArtifact turns a
// frame's keypoints/descriptors into the canonical FeatureArtifact payload
// (feature.schema.json, M0 JSON-array) and records the per-frame feature_sets
// row. The produced bytes are a pure function of the inputs, so identical runs
// dedupe in the CAS and replay from the ADR-020 task cache (AC-8).

#include <cstdint>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/scene/feature/feature_set.h"
#include "core/storage/metadata_db.h"

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
};

struct FeatureExtractionResult {
  spatial::core::FeatureSet feature_set;
  spatial::core::Uuid artifact_uuid{};
  std::string content_hash;
  bool deduplicated = false;
};

// Writes the FeatureArtifact payload + manifest into `store` and records the
// feature_sets row via `db`. The payload conforms to feature.schema.json; the
// producer guarantee count == keypoints.length == descriptors.length is
// enforced here (throws ProjectError kValidationDomain otherwise). The
// feature_set_id is deterministic (DeriveFeatureSetId), so re-execution on
// identical inputs is idempotent (RFC-0007 §6, AC-8).
FeatureExtractionResult WriteFeatureArtifact(
    spatial::core::ArtifactStore& store, spatial::core::MetadataDb& db,
    const WriteFeatureArtifactInput& input);

}  // namespace spatial::engine
