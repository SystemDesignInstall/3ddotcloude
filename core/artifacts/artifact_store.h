#pragma once

// Content-addressed artifact store (ADR-010, docs/specifications/
// artifact-format.md). Payloads live at artifacts/cas/<hash[0:2]>/<hash>;
// manifests at artifacts/<uuid>/manifest.json. Writes are atomic
// (temp-then-rename), payloads are immutable, identical content dedupes to a
// single CAS entry. Every read re-verifies SHA-256 before handing bytes out.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"

namespace spatial::core {

struct ArtifactWriteResult {
  std::string content_hash;
  Uuid artifact_uuid{};
  bool deduplicated = false;
};

class ArtifactStore {
 public:
  // `artifacts_root` is <project>/artifacts. `db` is the owning project's
  // metadata store; it must outlive this store.
  ArtifactStore(std::filesystem::path artifacts_root, MetadataDb& db);

  // Writes bytes atomically into the CAS store and records a manifest.
  // Returns the content hash; if identical content is already stored, the
  // write deduplicates (no new payload, no new manifest) and returns the
  // existing hash with deduplicated=true. Throws ArtifactError on failure.
  ArtifactWriteResult Put(const std::vector<std::uint8_t>& payload,
                          const ArtifactManifest& manifest);

  // Reads and verifies a payload. Re-verifies SHA-256(content) == hash
  // before returning; on mismatch the payload is quarantined and
  // ArtifactError is thrown. Returns nullopt when the hash is unknown.
  std::optional<std::vector<std::uint8_t>> Get(
      const std::string& content_hash);

  // True when the payload for `hash` exists and is on disk.
  bool Has(const std::string& content_hash) const;

  // True when the manifest directory exists.
  bool HasManifest(const Uuid& uuid) const;

  // The manifest for a stored artifact, if any.
  std::optional<ArtifactManifest> ReadManifest(const Uuid& uuid) const;

  // Number of distinct payloads stored on disk.
  std::size_t PayloadCount() const;

  // Garbage collection (artifact-format.md section 8).
  // dry_run=true returns a plan without deleting anything; commit=true
  // quarantines (atomic rename) then unlinks unreferenced payloads and their
  // manifests. Throws ArtifactGcConflict if a job is currently running
  // against the project.
  struct GcPlan {
    std::vector<std::string> unreferenced_hashes;
    std::vector<std::string> dangling_manifests;  // uuids
  };
  GcPlan GarbageCollect(bool dry_run);

  // Single-artifact reference check across the DB index.
  bool IsReferenced(const std::string& content_hash) const;

 private:
  std::filesystem::path PayloadPath(const std::string& hash) const;
  std::filesystem::path ManifestDir(const Uuid& uuid) const;
  std::filesystem::path ManifestPath(const Uuid& uuid) const;

  std::filesystem::path artifacts_root_;
  MetadataDb& db_;
};

}  // namespace spatial::core
