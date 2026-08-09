#pragma once

// Artifact manifest (schemas/json/artifact-manifest.schema.json,
// docs/specifications/artifact-format.md). Stored at
// artifacts/<artifact_uuid>/manifest.json. Metadata only; payloads live in
// the CAS store.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/utils/uuid.h"

namespace spatial::core {

struct ProducerInfo {
  std::string id;
  std::string version;
  std::string git_commit;
};

struct ArtifactManifest {
  Uuid artifact_uuid{};
  std::string content_hash;
  std::string type;
  std::int64_t schema_version = 1;
  ProducerInfo producer;
  std::vector<std::string> input_artifact_hashes;
  std::string configuration_hash;
  std::string creation_timestamp;
  std::string coordinate_frame;
  std::string unit = "meter";
  std::int64_t file_size = 0;
  std::string mime_type;
  std::string validation_status = "valid";

  // Image geometry (image.schema.json extension, RFC-0006 §8). Zero when the
  // artifact is not an image. Written by the importer from header-only
  // parsing (image-import.md §8); never decoded.
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::string pixel_format;

  // Reads and parses a manifest file. Throws ArtifactError on malformed
  // content (unreadable, missing required fields, or id mismatch).
  static ArtifactManifest Read(const std::filesystem::path& manifest_path);
};

// Serializes the manifest to a JSON string (canonical field order).
std::string ToJsonString(const ArtifactManifest& manifest);

// Parses a JSON string into a manifest. Throws ArtifactError on malformed
// content or missing required fields.
ArtifactManifest FromJsonString(const std::string& json);

}  // namespace spatial::core
