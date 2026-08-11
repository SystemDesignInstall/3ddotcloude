#pragma once

// FeatureSet — one frame's keypoints/descriptors (RFC-0007 §3). One per
// frame for a given detector: artifact_ref points at the FeatureArtifact
// payload in the CAS (feature.schema.json, type "feature"). Immutable once
// written (PPS-0001 §5.3, ADR-024); the id is deterministic so re-execution
// on identical inputs is idempotent (RFC-0007 §6, AC-8).

#include <cstdint>
#include <string>

#include "core/scene/identity.h"
#include "core/utils/uuid.h"

namespace spatial::core {

struct FeatureSet {
  Uuid feature_set_id{};    // UUIDv5, deterministic (DeriveFeatureSetId)
  Uuid frame_id{};
  std::string detector;         // canonical id, e.g. "mock" (RFC-0007 §2)
  std::string descriptor_type;  // e.g. "mock_16" (16-dim float rows)
  std::int64_t count = 0;       // == keypoints.length == descriptors.length
  Uuid artifact_ref{};          // FeatureArtifact artifact_uuid (CAS)
};

// FeatureSetID = UUIDv5(ns, "feature_set|<frame_id>|<content_hash>"). The
// same (frame, feature content) tuple maps to the same id across re-runs,
// machines, and processes (PPS-0001 §5.4).
inline Uuid DeriveFeatureSetId(const Uuid& frame_id,
                               const std::string& content_hash) {
  return GenerateUuidV5(PlatformIdentityNamespace(),
                        "feature_set|" + FormatUuid(frame_id) + "|" +
                            content_hash);
}

}  // namespace spatial::core
