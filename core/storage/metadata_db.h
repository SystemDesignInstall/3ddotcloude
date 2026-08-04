#pragma once

// SQLite WAL metadata store (ADR-008, ADR-009).
//
// project.db holds metadata and indices ONLY; payloads live in the
// content-addressed artifact store (core/artifacts). This class owns the
// connection, applies ratified migrations transactionally, and exposes a
// small typed surface for P1 (projects row + artifact index). Broader
// operational tables (observations, geometry, jobs, ...) are schema-resident
// but not yet exercised by code until their owning modules land.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/utils/uuid.h"

struct sqlite3;

namespace spatial::core {

struct ArtifactIndexRow {
  Uuid artifact_id;
  std::string content_hash;
  std::string type;
  std::int64_t schema_version = 1;
  std::string producer_json;
  std::string config_hash;
  std::int64_t created_at_ns = 0;
  std::string coordinate_frame;
  std::string unit;
  std::int64_t file_size = 0;
  std::string mime_type;
  std::string validation_status = "valid";
};

class MetadataDb {
 public:
  MetadataDb() = default;
  ~MetadataDb();
  MetadataDb(const MetadataDb&) = delete;
  MetadataDb& operator=(const MetadataDb&) = delete;
  MetadataDb(MetadataDb&&) noexcept;
  MetadataDb& operator=(MetadataDb&&) noexcept;

  // Creates the database file (with parent dirs) and applies all migrations.
  static MetadataDb Create(const std::filesystem::path& path);

  // Opens an existing database, verifies WAL mode, runs pending migrations.
  static MetadataDb Open(const std::filesystem::path& path);

  // Same as Open but with no write operations available.
  static MetadataDb OpenReadOnly(const std::filesystem::path& path);

  bool read_only() const noexcept { return read_only_; }
  bool IsOpen() const noexcept { return db_ != nullptr; }

  // Applies all registered migrations that have not yet been applied,
  // each inside its own transaction (ADR-009). Returns count applied.
  std::size_t ApplyMigrations();

  // P1: projects row.
  void InsertProject(const Uuid& project_id, const std::string& name,
                     std::int64_t schema_version,
                     const std::string& created_by_json,
                     std::int64_t created_at_ns, const std::string& default_crs,
                     const std::string& root_frame,
                     const std::string& flags_json,
                     const std::string& properties_json);

  // P1: artifact index (hash -> artifact record).
  void UpsertArtifact(const ArtifactIndexRow& row);
  std::optional<ArtifactIndexRow> FindArtifactByHash(
      const std::string& content_hash);
  std::vector<ArtifactIndexRow> FindArtifactsByType(
      const std::string& type) const;
  void RecordDependency(const std::string& input_hash,
                        const std::string& output_hash, const std::string& role);

  // Closes the connection.
  void Close();

 private:
  explicit MetadataDb(sqlite3* db, bool read_only);

  void Exec(const char* sql, const std::string& what);
  std::vector<std::string> AppliedMigrationIds();

  sqlite3* db_ = nullptr;
  bool read_only_ = false;
};

}  // namespace spatial::core
