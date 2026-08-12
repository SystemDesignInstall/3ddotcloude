#include "core/artifacts/artifact_manifest.h"

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"

namespace spatial::core {
namespace {

using json = nlohmann::json;

const std::string GetRequiredString(const json& j, const char* key,
                                    const std::string& what) {
  if (!j.contains(key) || !j[key].is_string()) {
    throw ArtifactError(ErrorCode::kArtifactManifest,
                        what + ": missing or non-string field '" + key + "'",
                        {}, false,
                        "The artifact manifest is malformed or corrupt.");
  }
  return j[key].get<std::string>();
}

}  // namespace

std::string ToJsonString(const ArtifactManifest& m) {
  json j;
  j["artifact_uuid"] = FormatUuid(m.artifact_uuid);
  j["content_hash"] = m.content_hash;
  j["type"] = m.type;
  j["schema_version"] = m.schema_version;
  j["producer"]["id"] = m.producer.id;
  j["producer"]["version"] = m.producer.version;
  j["producer"]["git_commit"] = m.producer.git_commit;
  j["input_artifact_hashes"] = m.input_artifact_hashes;
  // The schema allows string|null; empty means "no configuration recorded".
  j["configuration_hash"] = m.configuration_hash.empty()
                               ? json(nullptr)
                               : json(m.configuration_hash);
  j["creation_timestamp"] = m.creation_timestamp;
  j["coordinate_frame"] = m.coordinate_frame;
  j["unit"] = m.unit;
  j["file_size"] = m.file_size;
  j["mime_type"] = m.mime_type;
  j["validation_status"] = m.validation_status;
  j["width"] = m.width;
  j["height"] = m.height;
  j["pixel_format"] = m.pixel_format;
  return j.dump(2);
}

ArtifactManifest FromJsonString(const std::string& s) {
  json j = json::parse(s, nullptr, false);
  if (j.is_discarded()) {
    throw ArtifactError(ErrorCode::kArtifactManifest,
                        "artifact manifest is not valid JSON", {}, false,
                        "The artifact manifest is malformed or corrupt.");
  }
  ArtifactManifest m;
  m.artifact_uuid = ParseUuid(GetRequiredString(j, "artifact_uuid", "manifest"));
  m.content_hash = GetRequiredString(j, "content_hash", "manifest");
  m.type = GetRequiredString(j, "type", "manifest");
  if (j.contains("schema_version")) {
    m.schema_version = j["schema_version"].get<std::int64_t>();
  }
  if (j.contains("producer")) {
    const auto& p = j["producer"];
    if (p.contains("id")) m.producer.id = p["id"].get<std::string>();
    if (p.contains("version")) m.producer.version = p["version"].get<std::string>();
    if (p.contains("git_commit")) {
      m.producer.git_commit = p["git_commit"].get<std::string>();
    }
  }
  if (j.contains("input_artifact_hashes")) {
    for (const auto& h : j["input_artifact_hashes"]) {
      m.input_artifact_hashes.push_back(h.get<std::string>());
    }
  }
  if (j.contains("configuration_hash") &&
      j["configuration_hash"].is_string()) {
    m.configuration_hash = j["configuration_hash"].get<std::string>();
  }
  if (j.contains("creation_timestamp")) {
    m.creation_timestamp = j["creation_timestamp"].get<std::string>();
  }
  if (j.contains("coordinate_frame")) {
    m.coordinate_frame = j["coordinate_frame"].get<std::string>();
  }
  if (j.contains("unit")) m.unit = j["unit"].get<std::string>();
  if (j.contains("file_size")) m.file_size = j["file_size"].get<std::int64_t>();
  if (j.contains("mime_type")) m.mime_type = j["mime_type"].get<std::string>();
  if (j.contains("validation_status")) {
    m.validation_status = j["validation_status"].get<std::string>();
  }
  if (j.contains("width") && j["width"].is_number_integer()) {
    m.width = j["width"].get<std::int64_t>();
  }
  if (j.contains("height") && j["height"].is_number_integer()) {
    m.height = j["height"].get<std::int64_t>();
  }
  if (j.contains("pixel_format") && j["pixel_format"].is_string()) {
    m.pixel_format = j["pixel_format"].get<std::string>();
  }
  return m;
}

ArtifactManifest ArtifactManifest::Read(
    const std::filesystem::path& manifest_path) {
  if (!fs::Exists(manifest_path)) {
    throw ArtifactError(ErrorCode::kArtifactMissing,
                        "artifact manifest not found: " +
                            manifest_path.string(),
                        {}, false, "The artifact store is corrupt.");
  }
  return FromJsonString(fs::ReadText(manifest_path));
}

}  // namespace spatial::core
