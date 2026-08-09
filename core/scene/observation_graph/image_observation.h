#pragma once

// ImageObservation — an immutable record that a camera sensor captured an
// image at a time and place (scene-model.md §4.4, image-import.md §4).
// artifact_ref points at the ImageArtifact (CAS). Width/height/pixel_format
// come from the image header at import. Observations are never mutated
// (Architecture Principle 3); a correction is a new observation.

#include <cstdint>
#include <optional>
#include <string>

#include "core/coordinates/timestamp.h"
#include "core/utils/uuid.h"

namespace spatial::core {

struct ImageObservation {
  Uuid observation_id{};       // UUIDv5, deterministic (core/scene/identity.h)
  Uuid scene_id{};
  Uuid sensor_id{};
  Uuid frame_id{};             // kinematic frame (optional for stateless)
  Uuid session_id{};           // mandatory (RFC-0002, PPS-0001 §5.2)
  TimestampNs timestamp_ns{0};
  Uuid artifact_ref{};         // ImageArtifact artifact_uuid
  std::string source_json;     // SourceRef: importer/producer + version
  std::string properties_json; // typed extension data (foreign fields)

  // Image subtype fields (from header; EXIF-derived fields captured as
  // available into properties_json — image-import.md §5).
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::string pixel_format;
  std::optional<double> focal_prior_px;  // ADR-006 prior, never authoritative
  std::optional<std::string> band;       // e.g. "rgb", "nir"
};

}  // namespace spatial::core
