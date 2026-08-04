#pragma once

// Project Core (P1). A project is a directory with the .spx extension
// containing exactly six entries (ADR-008): project.json, project.db,
// artifacts/, cache/, logs/, temp/. project.json is the only file read
// before opening the database; no absolute paths are ever persisted
// (portability, ADR-008).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_store.h"
#include "core/storage/metadata_db.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"

namespace spatial::core {

struct CreatedBy {
  std::string app = "spatial-platform";
  std::string version = "0.1.0";
  std::string git_commit;
};

struct ProjectInfo {
  Uuid uuid{};
  std::string name;
  std::int64_t schema_version = 1;
  CreatedBy created_by;
  std::string created_at;
  std::string default_crs = "EPSG:4326";
  std::string root_frame = "root";
  bool read_only = false;
  bool encrypted = false;
  nlohmann::json properties = nlohmann::json::object();
};

class Project {
 public:
  Project() = default;
  ~Project() = default;
  Project(const Project&) = delete;
  Project& operator=(const Project&) = delete;
  Project(Project&&) noexcept;
  Project& operator=(Project&&) noexcept;

  // Creates a new project directory (with .spx extension), writes
  // project.json, initializes project.db (WAL, migrations), and the
  // artifacts/cache/logs/temp directories. Throws StorageError on failure.
  static Project Create(const std::filesystem::path& root,
                        const ProjectInfo& info);

  // Opens an existing project. Validates project.json, verifies the layout,
  // acquires the writer lock (unless read_only), and opens the metadata
  // database. Throws on any violation; the project is never silently repaired.
  static Project Open(const std::filesystem::path& root, bool read_only = false);

  const std::filesystem::path& root() const noexcept { return root_; }
  const ProjectInfo& info() const noexcept { return info_; }
  bool read_only() const noexcept { return read_only_; }
  bool IsOpen() const noexcept { return open_; }

  MetadataDb& db() { return db_; }
  const MetadataDb& db() const { return db_; }
  ArtifactStore& artifacts() { return *artifacts_; }
  const ArtifactStore& artifacts() const { return *artifacts_; }

  const std::filesystem::path& artifacts_root() const noexcept {
    return artifacts_root_;
  }
  const std::filesystem::path& cache_root() const noexcept { return cache_root_; }
  const std::filesystem::path& logs_root() const noexcept { return logs_root_; }
  const std::filesystem::path& temp_root() const noexcept { return temp_root_; }

  // Persists project.json (atomic). Read-only projects refuse.
  void Save();

  // Full integrity check (ADR-008): verifies every artifact hash and DB
  // consistency. Throws on the first violation with the offending hash/UUID.
  void VerifyIntegrity() const;

  void Close();

 private:
  static ProjectInfo ReadProjectJson(const std::filesystem::path& root);
  void WriteProjectJson() const;

  std::filesystem::path root_;
  std::filesystem::path artifacts_root_;
  std::filesystem::path cache_root_;
  std::filesystem::path logs_root_;
  std::filesystem::path temp_root_;
  std::filesystem::path db_path_;
  ProjectInfo info_;
  bool read_only_ = false;
  bool open_ = false;
  std::unique_ptr<FileLock> lock_;
  MetadataDb db_;
  std::unique_ptr<ArtifactStore> artifacts_;
};

}  // namespace spatial::core
