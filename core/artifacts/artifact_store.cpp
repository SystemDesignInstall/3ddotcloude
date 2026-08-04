#include "core/artifacts/artifact_store.h"

#include <algorithm>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"

namespace spatial::core {
namespace {

constexpr const char* kCasDir = "cas";
constexpr const char* kQuarantineDir = "quarantine";

}  // namespace

ArtifactStore::ArtifactStore(std::filesystem::path artifacts_root,
                             MetadataDb& db)
    : artifacts_root_(std::move(artifacts_root)), db_(db) {
  fs::CreateDirectories(artifacts_root_ / kCasDir);
  fs::CreateDirectories(artifacts_root_ / kQuarantineDir);
}

std::filesystem::path ArtifactStore::PayloadPath(
    const std::string& hash) const {
  return artifacts_root_ / kCasDir / hash.substr(0, 2) / hash;
}

std::filesystem::path ArtifactStore::ManifestDir(const Uuid& uuid) const {
  return artifacts_root_ / FormatUuid(uuid);
}

std::filesystem::path ArtifactStore::ManifestPath(const Uuid& uuid) const {
  return ManifestDir(uuid) / "manifest.json";
}

ArtifactWriteResult ArtifactStore::Put(
    const std::vector<std::uint8_t>& payload, const ArtifactManifest& manifest) {
  const std::string hash = Sha256Hex(payload);
  const auto existing = db_.FindArtifactByHash(hash);
  if (existing) {
    return ArtifactWriteResult{hash, existing->artifact_id, true};
  }

  const auto payload_path = PayloadPath(hash);
  if (!fs::Exists(payload_path)) {
    // Atomic write: temp-then-rename with fsync (ADR-010).
    fs::AtomicWrite(payload_path, payload);
  }

  auto manifest_copy = manifest;
  manifest_copy.content_hash = hash;
  manifest_copy.file_size = static_cast<std::int64_t>(payload.size());
  if (manifest_copy.creation_timestamp.empty()) {
    manifest_copy.creation_timestamp = fs::Iso8601UtcNow();
  }

  const Uuid artifact_uuid =
      manifest.artifact_uuid != Uuid{} ? manifest.artifact_uuid : GenerateUuid();
  manifest_copy.artifact_uuid = artifact_uuid;

  fs::CreateDirectories(ManifestDir(artifact_uuid));
  fs::AtomicWrite(ManifestPath(artifact_uuid), ToJsonString(manifest_copy));

  ArtifactIndexRow row;
  row.artifact_id = artifact_uuid;
  row.content_hash = hash;
  row.type = manifest_copy.type;
  row.schema_version = manifest_copy.schema_version;
  row.producer_json = nlohmann::json{{"id", manifest_copy.producer.id},
                                     {"version", manifest_copy.producer.version},
                                     {"git_commit",
                                      manifest_copy.producer.git_commit}}
                          .dump();
  row.config_hash = manifest_copy.configuration_hash;
  row.created_at_ns = fs::TimestampNsNow();
  row.coordinate_frame = manifest_copy.coordinate_frame;
  row.unit = manifest_copy.unit;
  row.file_size = manifest_copy.file_size;
  row.mime_type = manifest_copy.mime_type;
  row.validation_status = manifest_copy.validation_status;
  db_.UpsertArtifact(row);

  for (const auto& input : manifest_copy.input_artifact_hashes) {
    db_.RecordDependency(input, hash, "input");
  }

  return ArtifactWriteResult{hash, artifact_uuid, false};
}

std::optional<std::vector<std::uint8_t>> ArtifactStore::Get(
    const std::string& content_hash) {
  const auto payload_path = PayloadPath(content_hash);
  if (!fs::Exists(payload_path)) {
    return std::nullopt;
  }
  const auto bytes = fs::ReadFile(payload_path);
  const std::string actual = Sha256Hex(bytes);
  if (actual != content_hash) {
    // Quarantine the corrupt payload; flag the manifest degraded.
    const auto quarantine_root = artifacts_root_ / kQuarantineDir;
    fs::CreateDirectories(quarantine_root);
    const auto quarantine_path =
        quarantine_root / (content_hash + ".corrupt");
    if (!fs::Exists(quarantine_path)) {
      fs::Rename(payload_path, quarantine_path);
    }
    const auto index = db_.FindArtifactByHash(content_hash);
    if (index) {
      auto degraded = *index;
      degraded.validation_status = "degraded";
      db_.UpsertArtifact(degraded);
    }
    throw ArtifactError(ErrorCode::kArtifactHashMismatch,
                        "artifact integrity check failed for hash: " +
                            content_hash,
                        {{"content_hash", content_hash}},
                        false,
                        "The payload is corrupt; it was quarantined.");
  }
  return bytes;
}

bool ArtifactStore::Has(const std::string& content_hash) const {
  return fs::Exists(PayloadPath(content_hash));
}

bool ArtifactStore::HasManifest(const Uuid& uuid) const {
  return fs::Exists(ManifestPath(uuid));
}

std::optional<ArtifactManifest> ArtifactStore::ReadManifest(
    const Uuid& uuid) const {
  const auto path = ManifestPath(uuid);
  if (!fs::Exists(path)) return std::nullopt;
  return ArtifactManifest::Read(path);
}

std::size_t ArtifactStore::PayloadCount() const {
  std::size_t count = 0;
  for (const auto& shard : fs::ListDirectories(artifacts_root_ / kCasDir)) {
    count += fs::ListFiles(shard).size();
  }
  return count;
}

bool ArtifactStore::IsReferenced(const std::string& content_hash) const {
  return db_.FindArtifactByHash(content_hash).has_value();
}

ArtifactStore::GcPlan ArtifactStore::GarbageCollect(bool dry_run) {
  // Collect all on-disk hashes.
  std::set<std::string> on_disk;
  for (const auto& shard : fs::ListDirectories(artifacts_root_ / kCasDir)) {
    for (const auto& file : fs::ListFiles(shard)) {
      on_disk.insert(file.filename().string());
    }
  }

  // Anything not referenced by the DB index is unreferenced.
  std::vector<std::string> unreferenced;
  for (const auto& hash : on_disk) {
    if (!IsReferenced(hash)) unreferenced.push_back(hash);
  }

  // Manifest dirs (uuid-named) whose artifact is unreferenced are dangling.
  std::vector<std::string> dangling;
  for (const auto& dir : fs::ListDirectories(artifacts_root_)) {
    const auto name = dir.filename().string();
    if (name == kCasDir || name == kQuarantineDir) continue;
    Uuid uuid;
    try {
      uuid = ParseUuid(name);
    } catch (const ValidationError&) {
      continue;
    }
    const auto manifest = ReadManifest(uuid);
    if (manifest && !IsReferenced(manifest->content_hash)) {
      dangling.push_back(name);
    }
  }

  GcPlan plan;
  plan.unreferenced_hashes = unreferenced;
  plan.dangling_manifests = dangling;
  if (dry_run) return plan;

  // Quarantine then unlink (atomic-rename-first, artifact-format.md section 8).
  const auto quarantine_root = artifacts_root_ / kQuarantineDir;
  fs::CreateDirectories(quarantine_root);
  for (const auto& hash : unreferenced) {
    const auto payload = PayloadPath(hash);
    if (!fs::Exists(payload)) continue;
    const auto quarantined = quarantine_root / (hash + ".gc");
    fs::Rename(payload, quarantined);
    fs::RemoveAll(quarantined);
  }
  for (const auto& uuid_str : dangling) {
    fs::RemoveAll(artifacts_root_ / uuid_str);
  }
  return plan;
}

}  // namespace spatial::core
