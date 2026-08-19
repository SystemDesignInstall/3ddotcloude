#include "core/storage/metadata_db.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#include <sqlite3.h>

#include "core/errors/project_error.h"
#include "storage/migrations_generated.h"
#include "core/utils/fs.h"

namespace spatial::core {
namespace {

constexpr const char* kSchemaMeta =
    "CREATE TABLE IF NOT EXISTS schema_meta ("
    "  version    INTEGER PRIMARY KEY,"
    "  applied_at TEXT    NOT NULL DEFAULT (datetime('now'))"
    ");";

}  // namespace

namespace {
// Forward declarations: the artifact-row reader is defined with the other row
// readers below but used by the artifact-index accessors above them.
ArtifactIndexRow ReadArtifactRow(sqlite3_stmt* stmt);
}  // namespace

MetadataDb::MetadataDb(sqlite3* db, bool read_only)
    : db_(db), read_only_(read_only) {}

MetadataDb::~MetadataDb() { Close(); }

MetadataDb::MetadataDb(MetadataDb&& other) noexcept
    : db_(other.db_), read_only_(other.read_only_) {
  other.db_ = nullptr;
}

MetadataDb& MetadataDb::operator=(MetadataDb&& other) noexcept {
  if (this != &other) {
    Close();
    db_ = other.db_;
    read_only_ = other.read_only_;
    other.db_ = nullptr;
  }
  return *this;
}

void MetadataDb::Close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

void MetadataDb::Exec(const char* sql, const std::string& what) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    const std::string msg = err ? err : "unknown sqlite error";
    sqlite3_free(err);
    throw SchemaError(ErrorCode::kSchemaInvalid, what + ": " + msg, {},
                      false, "The project metadata database is corrupt.");
  }
}

std::vector<std::string> MetadataDb::AppliedMigrationIds() {
  std::vector<std::string> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT version FROM schema_meta ORDER BY 1";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot read applied migrations");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(std::to_string(sqlite3_column_int64(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return out;
}

MetadataDb MetadataDb::Create(const std::filesystem::path& path) {
  fs::CreateDirectories(path.parent_path());
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE |
                                                   SQLITE_OPEN_CREATE |
                                                   SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    const std::string msg = db ? sqlite3_errmsg(db) : "unknown";
    if (db) sqlite3_close(db);
    throw StorageError(ErrorCode::kStorageIo,
                       "cannot create metadata database: " + path.string() +
                           ": " + msg,
                       {}, false, "Check filesystem permissions.");
  }
  MetadataDb out(db, false);
  out.Exec("PRAGMA journal_mode=WAL;", "enable WAL");
  out.Exec("PRAGMA foreign_keys=ON;", "enable foreign keys");
  out.Exec(kSchemaMeta, "ensure schema_meta");
  out.ApplyMigrations();
  return out;
}

MetadataDb MetadataDb::Open(const std::filesystem::path& path) {
  if (!fs::Exists(path)) {
    throw StorageError(ErrorCode::kProjectNotFound,
                       "metadata database not found: " + path.string());
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.string().c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) !=
      SQLITE_OK) {
    const std::string msg = db ? sqlite3_errmsg(db) : "unknown";
    if (db) sqlite3_close(db);
    throw StorageError(ErrorCode::kStorageIo,
                       "cannot open metadata database: " + path.string() +
                           ": " + msg,
                       {}, false, "The project metadata database is corrupt.");
  }
  MetadataDb out(db, false);
  out.Exec("PRAGMA journal_mode=WAL;", "enable WAL");
  out.Exec("PRAGMA foreign_keys=ON;", "enable foreign keys");
  out.Exec(kSchemaMeta, "ensure schema_meta");
  out.ApplyMigrations();
  return out;
}

MetadataDb MetadataDb::OpenReadOnly(const std::filesystem::path& path) {
  if (!fs::Exists(path)) {
    throw StorageError(ErrorCode::kProjectNotFound,
                       "metadata database not found: " + path.string());
  }
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.string().c_str(), &db,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) !=
      SQLITE_OK) {
    const std::string msg = db ? sqlite3_errmsg(db) : "unknown";
    if (db) sqlite3_close(db);
    throw StorageError(ErrorCode::kStorageIo,
                       "cannot open metadata database read-only: " +
                           path.string() + ": " + msg,
                       {}, false, "The project metadata database is corrupt.");
  }
  MetadataDb out(db, true);
  out.Exec(kSchemaMeta, "ensure schema_meta");
  return out;
}

std::size_t MetadataDb::ApplyMigrations() {
  if (read_only_) return 0;
  const auto applied = AppliedMigrationIds();
  const bool has_0001 =
      std::find(applied.begin(), applied.end(), "1") != applied.end();
  const bool has_0003 =
      std::find(applied.begin(), applied.end(), "3") != applied.end();
  const bool has_0004 =
      std::find(applied.begin(), applied.end(), "4") != applied.end();
  const bool has_0005 =
      std::find(applied.begin(), applied.end(), "5") != applied.end();
  const bool has_0006 =
      std::find(applied.begin(), applied.end(), "6") != applied.end();
  const bool has_0007 =
      std::find(applied.begin(), applied.end(), "7") != applied.end();
  const bool has_0008 =
      std::find(applied.begin(), applied.end(), "8") != applied.end();
  std::size_t count = 0;

  // Migration 0001 (initial schema, ratified; identical to HEAD schema.sql).
  // The script itself no longer records the version (it is stripped at
  // build time); the runner records schema version 1 in schema_meta.
  if (!has_0001) {
    Exec("BEGIN IMMEDIATE TRANSACTION;", "begin migration 0001");
    try {
      Exec(generated::kMigration0001Sql, "apply migration 0001");
      Exec("INSERT INTO schema_meta (version) VALUES (1);",
           "record schema version 1");
      Exec("COMMIT;", "commit migration 0001");
    } catch (...) {
      Exec("ROLLBACK;", "rollback migration 0001");
      throw;
    }
    ++count;
  }

  // Migration 0003 (RFC-0003 engine scheduler state). 0002 is claimed by
  // RFC-0002 (scene data model) and does not exist yet; numbering skips it.
  if (!has_0003) {
    Exec("BEGIN IMMEDIATE TRANSACTION;", "begin migration 0003");
    try {
      Exec(generated::kMigration0003Sql, "apply migration 0003");
      Exec("INSERT INTO schema_meta (version) VALUES (3);",
           "record schema version 3");
      Exec("COMMIT;", "commit migration 0003");
    } catch (...) {
      Exec("ROLLBACK;", "rollback migration 0003");
      throw;
    }
    ++count;
  }

  // Migration 0004 (RFC-0003 P1.4, ExecutionManifest: pipeline-level
  // execution document). Owned by engine/pipeline; the quality_report_id
  // column is linked by the RFC-0005 validate stage.
  if (!has_0004) {
    Exec("BEGIN IMMEDIATE TRANSACTION;", "begin migration 0004");
    try {
      Exec(generated::kMigration0004Sql, "apply migration 0004");
      Exec("INSERT INTO schema_meta (version) VALUES (4);",
           "record schema version 4");
      Exec("COMMIT;", "commit migration 0004");
    } catch (...) {
      Exec("ROLLBACK;", "rollback migration 0004");
      throw;
    }
    ++count;
  }

  // Migration 0005 (RFC-0006 §14, P2.1 review debt #8): persistent
  // provenance for rejected import inputs.
  if (!has_0005) {
    Exec("BEGIN IMMEDIATE TRANSACTION;", "begin migration 0005");
    try {
      Exec(generated::kMigration0005Sql, "apply migration 0005");
      Exec("INSERT INTO schema_meta (version) VALUES (5);",
           "record schema version 5");
      Exec("COMMIT;", "commit migration 0005");
    } catch (...) {
      Exec("ROLLBACK;", "rollback migration 0005");
      throw;
    }
    ++count;
  }

  // Migration 0006 (RFC-0002 §3.1 + RFC-0006 §9, P2.2): calibration
  // validity intervals on the calibrations table (half-open
  // [valid_from_ns, valid_to_ns), valid_to_ns IS NULL = open-ended).
  if (!has_0006) {
    Exec("BEGIN IMMEDIATE TRANSACTION;", "begin migration 0006");
    try {
      Exec(generated::kMigration0006Sql, "apply migration 0006");
      Exec("INSERT INTO schema_meta (version) VALUES (6);",
           "record schema version 6");
      Exec("COMMIT;", "commit migration 0006");
    } catch (...) {
      Exec("ROLLBACK;", "rollback migration 0006");
      throw;
    }
    ++count;
  }

  // Migration 0007 (P2.5, D-CRM-15): canonical reconstruction scene entity.
  // Stores reconstruction metadata for query access; the full Reconstruction
  // document lives in the CAS payload.
  if (!has_0007) {
    Exec("BEGIN IMMEDIATE TRANSACTION;", "begin migration 0007");
    try {
      Exec(generated::kMigration0007Sql, "apply migration 0007");
      Exec("INSERT INTO schema_meta (version) VALUES (7);",
           "record schema version 7");
      Exec("COMMIT;", "commit migration 0007");
    } catch (...) {
      Exec("ROLLBACK;", "rollback migration 0007");
      throw;
    }
    ++count;
  }

  // Migration 0008 (P3-impl-1): canonical trajectory / pose graph / loop
  // closure / optimization result tables.
  if (!has_0008) {
    Exec("BEGIN IMMEDIATE TRANSACTION;", "begin migration 0008");
    try {
      Exec(generated::kMigration0008Sql, "apply migration 0008");
      Exec("INSERT INTO schema_meta (version) VALUES (8);",
           "record schema version 8");
      Exec("COMMIT;", "commit migration 0008");
    } catch (...) {
      Exec("ROLLBACK;", "rollback migration 0008");
      throw;
    }
    ++count;
  }
  return count;
}

void MetadataDb::InsertProject(const Uuid& project_id, const std::string& name,
                               std::int64_t schema_version,
                               const std::string& created_by_json,
                               std::int64_t created_at_ns,
                               const std::string& default_crs,
                               const std::string& root_frame,
                               const std::string& flags_json,
                               const std::string& properties_json) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO projects (project_id, name, schema_version, created_by_json,"
      " created_at_ns, default_crs, root_frame, flags_json, properties_json)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid, "cannot prepare insert project");
  }
  sqlite3_bind_blob(stmt, 1, project_id.data(), static_cast<int>(project_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, schema_version);
  sqlite3_bind_text(stmt, 4, created_by_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, created_at_ns);
  sqlite3_bind_text(stmt, 6, default_crs.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, root_frame.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, flags_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, properties_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert project row: " + msg);
  }
  sqlite3_finalize(stmt);
}

void MetadataDb::UpsertArtifact(const ArtifactIndexRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT OR REPLACE INTO artifacts (artifact_id, content_hash, type,"
      " schema_version, producer_json, config_hash, created_at_ns,"
      " coordinate_frame, unit, file_size, mime_type, validation_status)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare upsert artifact");
  }
  sqlite3_bind_blob(stmt, 1, row.artifact_id.data(),
                    static_cast<int>(row.artifact_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, row.content_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, row.type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, row.schema_version);
  sqlite3_bind_text(stmt, 5, row.producer_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, row.config_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, row.created_at_ns);
  sqlite3_bind_text(stmt, 8, row.coordinate_frame.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, row.unit.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 10, row.file_size);
  sqlite3_bind_text(stmt, 11, row.mime_type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 12, row.validation_status.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot upsert artifact row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::optional<ArtifactIndexRow> MetadataDb::FindArtifactByHash(
    const std::string& content_hash) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT artifact_id, content_hash, type, schema_version,"
                    " producer_json, config_hash, created_at_ns, coordinate_frame,"
                    " unit, file_size, mime_type, validation_status"
                    " FROM artifacts WHERE content_hash = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find artifact");
  }
  sqlite3_bind_text(stmt, 1, content_hash.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<ArtifactIndexRow> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out = ReadArtifactRow(stmt);
  }
  sqlite3_finalize(stmt);
  return out;
}

std::optional<ArtifactIndexRow> MetadataDb::FindArtifactById(
    const Uuid& artifact_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT artifact_id, content_hash, type, schema_version,"
                    " producer_json, config_hash, created_at_ns, coordinate_frame,"
                    " unit, file_size, mime_type, validation_status"
                    " FROM artifacts WHERE artifact_id = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find artifact by id");
  }
  sqlite3_bind_blob(stmt, 1, artifact_id.data(),
                    static_cast<int>(artifact_id.size()), SQLITE_TRANSIENT);
  std::optional<ArtifactIndexRow> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out = ReadArtifactRow(stmt);
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ArtifactIndexRow> MetadataDb::FindArtifactsByType(
    const std::string& type) const {
  std::vector<ArtifactIndexRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT artifact_id, content_hash, type, schema_version,"
                    " producer_json, config_hash, created_at_ns, coordinate_frame,"
                    " unit, file_size, mime_type, validation_status"
                    " FROM artifacts WHERE type = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find artifacts by type");
  }
  sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(ReadArtifactRow(stmt));
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::RecordDependency(const std::string& input_hash,
                                  const std::string& output_hash,
                                  const std::string& role) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT OR REPLACE INTO artifact_dependencies (input_hash, output_hash,"
      " role) VALUES (?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare record dependency");
  }
  sqlite3_bind_text(stmt, 1, input_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, output_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, role.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot record dependency: " + msg);
  }
  sqlite3_finalize(stmt);
}

namespace {

// Binds a nil UUID as SQL NULL, otherwise as a 16-byte BLOB (the SQLite
// convention for UUID columns, schema.sql). Nil == "absent reference".
void BindUuidOrNull(sqlite3_stmt* stmt, int index, const Uuid& uuid) {
  if (IsNil(uuid)) {
    sqlite3_bind_null(stmt, index);
  } else {
    sqlite3_bind_blob(stmt, index, uuid.data(), static_cast<int>(uuid.size()),
                      SQLITE_TRANSIENT);
  }
}

// Reads a 16-byte UUID column; returns nullopt for NULL or malformed size.
std::optional<Uuid> ColumnUuid(sqlite3_stmt* stmt, int col) {
  const auto* blob = sqlite3_column_blob(stmt, col);
  const int size = sqlite3_column_bytes(stmt, col);
  if (blob && size == 16) {
    Uuid out{};
    std::copy_n(static_cast<const std::uint8_t*>(blob), 16, out.begin());
    return out;
  }
  return std::nullopt;
}

// Reads a nullable INTEGER column; returns nullopt for SQL NULL.
std::optional<std::int64_t> ColumnInt64OrNull(sqlite3_stmt* stmt, int col) {
  if (sqlite3_column_type(stmt, col) == SQLITE_NULL) return std::nullopt;
  return sqlite3_column_int64(stmt, col);
}

// Reads an artifacts row (SELECT column order: artifact_id, content_hash,
// type, schema_version, producer_json, config_hash, created_at_ns,
// coordinate_frame, unit, file_size, mime_type, validation_status).
ArtifactIndexRow ReadArtifactRow(sqlite3_stmt* stmt) {
  ArtifactIndexRow row;
  if (const auto* blob = sqlite3_column_blob(stmt, 0);
      blob && sqlite3_column_bytes(stmt, 0) == 16) {
    std::copy_n(static_cast<const std::uint8_t*>(blob), 16,
                row.artifact_id.begin());
  }
  if (const auto* t = sqlite3_column_text(stmt, 1)) {
    row.content_hash = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 2)) {
    row.type = reinterpret_cast<const char*>(t);
  }
  row.schema_version = sqlite3_column_int64(stmt, 3);
  if (const auto* t = sqlite3_column_text(stmt, 4)) {
    row.producer_json = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 5)) {
    row.config_hash = reinterpret_cast<const char*>(t);
  }
  row.created_at_ns = sqlite3_column_int64(stmt, 6);
  if (const auto* t = sqlite3_column_text(stmt, 7)) {
    row.coordinate_frame = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 8)) {
    row.unit = reinterpret_cast<const char*>(t);
  }
  row.file_size = sqlite3_column_int64(stmt, 9);
  if (const auto* t = sqlite3_column_text(stmt, 10)) {
    row.mime_type = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 11)) {
    row.validation_status = reinterpret_cast<const char*>(t);
  }
  return row;
}

// Reads a scene_versions row (SELECT column order: version_id, scene_id,
// parent_version_id, stage, created_by_json, created_at_ns, status).
SceneVersionRow ReadSceneVersionRow(sqlite3_stmt* stmt) {
  SceneVersionRow row;
  if (const auto u = ColumnUuid(stmt, 0)) row.version_id = *u;
  if (const auto u = ColumnUuid(stmt, 1)) row.scene_id = *u;
  if (const auto u = ColumnUuid(stmt, 2)) row.parent_version_id = *u;
  if (const auto* t = sqlite3_column_text(stmt, 3)) {
    row.stage = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 4)) {
    row.created_by_json = reinterpret_cast<const char*>(t);
  }
  row.created_at_ns = sqlite3_column_int64(stmt, 5);
  if (const auto* t = sqlite3_column_text(stmt, 6)) {
    row.status = reinterpret_cast<const char*>(t);
  }
  return row;
}

// Reads a frames row (SELECT column order: frame_id, scene_id, session_id,
// timestamp_ns, sequence_index, sensor_id, pose_ref, properties_json).
FrameRow ReadFrameRow(sqlite3_stmt* stmt) {
  FrameRow row;
  if (const auto u = ColumnUuid(stmt, 0)) row.frame_id = *u;
  if (const auto u = ColumnUuid(stmt, 1)) row.scene_id = *u;
  if (const auto u = ColumnUuid(stmt, 2)) row.session_id = *u;
  row.timestamp_ns = sqlite3_column_int64(stmt, 3);
  row.sequence_index = sqlite3_column_int64(stmt, 4);
  if (const auto u = ColumnUuid(stmt, 5)) row.sensor_id = *u;
  if (const auto u = ColumnUuid(stmt, 6)) row.pose_ref = *u;
  if (const auto* t = sqlite3_column_text(stmt, 7)) {
    row.properties_json = reinterpret_cast<const char*>(t);
  }
  return row;
}

// Reads an observations row (SELECT column order: observation_id, scene_id,
// sensor_id, frame_id, session_id, timestamp_ns, type, artifact_ref,
// source_json, properties_json, width, height, pixel_format; the last three
// come from the LEFT JOINed observation_payloads).
ObservationRow ReadObservationRow(sqlite3_stmt* stmt) {
  ObservationRow row;
  if (const auto u = ColumnUuid(stmt, 0)) row.observation_id = *u;
  if (const auto u = ColumnUuid(stmt, 1)) row.scene_id = *u;
  if (const auto u = ColumnUuid(stmt, 2)) row.sensor_id = *u;
  if (const auto u = ColumnUuid(stmt, 3)) row.frame_id = *u;
  if (const auto u = ColumnUuid(stmt, 4)) row.session_id = *u;
  row.timestamp_ns = sqlite3_column_int64(stmt, 5);
  if (const auto* t = sqlite3_column_text(stmt, 6)) {
    row.type = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 7)) {
    row.artifact_ref = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 8)) {
    row.source_json = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 9)) {
    row.properties_json = reinterpret_cast<const char*>(t);
  }
  if (sqlite3_column_type(stmt, 10) != SQLITE_NULL) {
    row.width = sqlite3_column_int64(stmt, 10);
  }
  if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
    row.height = sqlite3_column_int64(stmt, 11);
  }
  if (const auto* t = sqlite3_column_text(stmt, 12)) {
    row.pixel_format = reinterpret_cast<const char*>(t);
  }
  return row;
}

// Reads a feature_sets row (SELECT column order: feature_set_id, frame_id,
// detector, descriptor_type, count, artifact_ref).
FeatureSetRow ReadFeatureSetRow(sqlite3_stmt* stmt) {
  FeatureSetRow row;
  if (const auto u = ColumnUuid(stmt, 0)) row.feature_set_id = *u;
  if (const auto u = ColumnUuid(stmt, 1)) row.frame_id = *u;
  if (const auto* t = sqlite3_column_text(stmt, 2)) {
    row.detector = reinterpret_cast<const char*>(t);
  }
  if (const auto* t = sqlite3_column_text(stmt, 3)) {
    row.descriptor_type = reinterpret_cast<const char*>(t);
  }
  row.count = sqlite3_column_int64(stmt, 4);
  if (const auto* t = sqlite3_column_text(stmt, 5)) {
    row.artifact_ref = reinterpret_cast<const char*>(t);
  }
  return row;
}

}  // namespace

std::optional<Uuid> MetadataDb::FindSession(const Uuid& session_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT session_id FROM capture_sessions WHERE session_id = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find session");
  }
  sqlite3_bind_blob(stmt, 1, session_id.data(),
                    static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
  std::optional<Uuid> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out = ColumnUuid(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::InsertCaptureSession(const CaptureSessionRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO capture_sessions (session_id, project_id, name,"
      " started_at_ns, ended_at_ns, source_uri, status, provenance_json)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert capture session");
  }
  sqlite3_bind_blob(stmt, 1, row.session_id.data(),
                    static_cast<int>(row.session_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.project_id.data(),
                    static_cast<int>(row.project_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, row.name.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, row.started_at_ns);
  sqlite3_bind_int64(stmt, 5, row.ended_at_ns);
  sqlite3_bind_text(stmt, 6, row.source_uri.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, row.status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, row.provenance_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert capture session row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::optional<SensorRow> MetadataDb::FindSensor(const Uuid& sensor_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT s.sensor_id, s.project_id, s.type, s.manufacturer, s.model,"
      " s.serial_number, s.time_domain, s.calibration_id, s.rig_id,"
      " s.source_json, s.status,"
      " EXISTS (SELECT 1 FROM calibrations c WHERE c.sensor_id = s.sensor_id)"
      " FROM sensors s WHERE s.sensor_id = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find sensor");
  }
  sqlite3_bind_blob(stmt, 1, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  std::optional<SensorRow> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    SensorRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.sensor_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.project_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 2)) {
      row.type = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 3)) {
      row.manufacturer = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 4)) {
      row.model = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      row.serial_number = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 6)) {
      row.time_domain = reinterpret_cast<const char*>(t);
    }
    if (const auto u = ColumnUuid(stmt, 7)) row.calibration_id = *u;
    if (const auto u = ColumnUuid(stmt, 8)) row.rig_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 9)) {
      row.source_json = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 10)) {
      row.status = reinterpret_cast<const char*>(t);
    }
    // calibrations.backlink column predates a canonical calibration_id link on
    // sensors; the EXISTS is authoritative for calibration presence.
    row.has_calibration = sqlite3_column_int(stmt, 11) != 0;
    out = row;
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::RegisterSensor(const SensorRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO sensors (sensor_id, project_id, type, manufacturer, model,"
      " serial_number, time_domain, calibration_id, rig_id, source_json,"
      " status) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare register sensor");
  }
  sqlite3_bind_blob(stmt, 1, row.sensor_id.data(),
                    static_cast<int>(row.sensor_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.project_id.data(),
                    static_cast<int>(row.project_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, row.type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, row.manufacturer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, row.model.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, row.serial_number.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, row.time_domain.c_str(), -1, SQLITE_TRANSIENT);
  BindUuidOrNull(stmt, 8, row.calibration_id);
  BindUuidOrNull(stmt, 9, row.rig_id);
  sqlite3_bind_text(stmt, 10, row.source_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11, row.status.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_CONSTRAINT) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "sensor already registered: " + FormatUuid(row.sensor_id) +
                          ": " + msg);
  }
  if (rc != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert sensor row: " + msg);
  }
  sqlite3_finalize(stmt);
}

void MetadataDb::UpdateSensorMetadata(const Uuid& sensor_id,
                                      const SensorMetadataUpdate& metadata) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* exists = nullptr;
  const char* exists_sql = "SELECT 1 FROM sensors WHERE sensor_id = ?";
  if (sqlite3_prepare_v2(db_, exists_sql, -1, &exists, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find sensor for update");
  }
  sqlite3_bind_blob(exists, 1, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  const bool present = sqlite3_step(exists) == SQLITE_ROW;
  sqlite3_finalize(exists);
  if (!present) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "no such sensor: " + FormatUuid(sensor_id));
  }

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE sensors SET manufacturer = ?, model = ?, serial_number = ?,"
      " status = ? WHERE sensor_id = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare update sensor metadata");
  }
  const auto bind_text_or_null = [stmt](int index, const std::string& value) {
    if (value.empty()) {
      sqlite3_bind_null(stmt, index);
    } else {
      sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
    }
  };
  bind_text_or_null(1, metadata.manufacturer);
  bind_text_or_null(2, metadata.model);
  bind_text_or_null(3, metadata.serial_number);
  bind_text_or_null(4, metadata.status);
  sqlite3_bind_blob(stmt, 5, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot update sensor metadata: " + msg);
  }
  sqlite3_finalize(stmt);
}

void MetadataDb::AddCalibration(const CalibrationRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  if (!row.valid_from_ns.has_value()) {
    throw CalibrationError(ErrorCode::kCalibrationInvalid,
                           "calibration requires a valid_from_ns interval "
                           "start (sensor-model.md §3.1)");
  }
  if (row.valid_to_ns.has_value() &&
      *row.valid_to_ns <= *row.valid_from_ns) {
    throw CalibrationError(ErrorCode::kCalibrationInvalid,
                           "valid_to_ns must be greater than valid_from_ns");
  }
  constexpr std::int64_t kInf = std::numeric_limits<std::int64_t>::max();

  Exec("BEGIN IMMEDIATE TRANSACTION;", "begin add calibration");
  try {
    // At most one calibration may be valid per sensor per timestamp. Overlap
    // test on half-open [a, b): intervals overlap iff a < d && c < b, with a
    // NULL end treated as +inf.
    sqlite3_stmt* overlap = nullptr;
    const char* overlap_sql =
        "SELECT 1 FROM calibrations WHERE sensor_id = ?"
        " AND valid_from_ns IS NOT NULL"
        " AND valid_from_ns < ?"
        " AND COALESCE(valid_to_ns, ?) > ? LIMIT 1";
    if (sqlite3_prepare_v2(db_, overlap_sql, -1, &overlap, nullptr) !=
        SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare calibration overlap check");
    }
    sqlite3_bind_blob(overlap, 1, row.sensor_id.data(),
                      static_cast<int>(row.sensor_id.size()), SQLITE_TRANSIENT);
    // Half-open overlap test: intervals [a,b) and [c,d) overlap iff
    // a < d && c < b, with a NULL end treated as +inf.
    if (row.valid_to_ns.has_value()) {
      sqlite3_bind_int64(overlap, 2, *row.valid_to_ns);
    } else {
      sqlite3_bind_int64(overlap, 2, kInf);
    }
    sqlite3_bind_int64(overlap, 3, kInf);
    sqlite3_bind_int64(overlap, 4, *row.valid_from_ns);
    const bool overlaps = sqlite3_step(overlap) == SQLITE_ROW;
    sqlite3_finalize(overlap);
    if (overlaps) {
      throw CalibrationError(
          ErrorCode::kCalibrationInvalid,
          "calibration interval overlaps an existing calibration for sensor " +
              FormatUuid(row.sensor_id));
    }

    // Append-only versioning: version = previous max + 1, never rewritten.
    sqlite3_stmt* ver = nullptr;
    const char* ver_sql =
        "SELECT COALESCE(MAX(version), 0) + 1 FROM calibrations"
        " WHERE sensor_id = ?";
    if (sqlite3_prepare_v2(db_, ver_sql, -1, &ver, nullptr) != SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare calibration version");
    }
    sqlite3_bind_blob(ver, 1, row.sensor_id.data(),
                      static_cast<int>(row.sensor_id.size()), SQLITE_TRANSIENT);
    std::int64_t version = 1;
    if (sqlite3_step(ver) == SQLITE_ROW) {
      version = sqlite3_column_int64(ver, 0);
    }
    sqlite3_finalize(ver);

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO calibrations (calibration_id, sensor_id, version,"
        " calibration_time_ns, source, intrinsics_json, distortion_json,"
        " extrinsics_json, uncertainty_json, valid_from_ns, valid_to_ns)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare insert calibration");
    }
    sqlite3_bind_blob(stmt, 1, row.calibration_id.data(),
                      static_cast<int>(row.calibration_id.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, row.sensor_id.data(),
                      static_cast<int>(row.sensor_id.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, version);
    sqlite3_bind_int64(stmt, 4, row.calibration_time_ns);
    sqlite3_bind_text(stmt, 5, row.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, row.intrinsics_json.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, row.distortion_json.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, row.extrinsics_json.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, row.uncertainty_json.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 10, *row.valid_from_ns);
    if (row.valid_to_ns.has_value()) {
      sqlite3_bind_int64(stmt, 11, *row.valid_to_ns);
    } else {
      sqlite3_bind_null(stmt, 11);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      const std::string msg = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot insert calibration row: " + msg);
    }
    sqlite3_finalize(stmt);

    // Maintain the sensors.calibration_id "latest" pointer (never used by
    // ResolveCalibrationAt; historical resolution reads intervals only).
    sqlite3_stmt* ptr = nullptr;
    const char* ptr_sql =
        "UPDATE sensors SET calibration_id = ? WHERE sensor_id = ?";
    if (sqlite3_prepare_v2(db_, ptr_sql, -1, &ptr, nullptr) != SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare calibration pointer update");
    }
    sqlite3_bind_blob(ptr, 1, row.calibration_id.data(),
                      static_cast<int>(row.calibration_id.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_blob(ptr, 2, row.sensor_id.data(),
                      static_cast<int>(row.sensor_id.size()), SQLITE_TRANSIENT);
    sqlite3_step(ptr);
    sqlite3_finalize(ptr);

    Exec("COMMIT;", "commit add calibration");
  } catch (...) {
    Exec("ROLLBACK;", "rollback add calibration");
    throw;
  }
}

std::optional<CalibrationRow> MetadataDb::ResolveCalibrationAt(
    const Uuid& sensor_id, std::int64_t timestamp_ns) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT calibration_id, sensor_id, version, calibration_time_ns, source,"
      " intrinsics_json, distortion_json, extrinsics_json, uncertainty_json,"
      " valid_from_ns, valid_to_ns"
      " FROM calibrations WHERE sensor_id = ?"
      " AND valid_from_ns IS NOT NULL AND valid_from_ns <= ?"
      " AND (valid_to_ns IS NULL OR valid_to_ns > ?)"
      " ORDER BY valid_from_ns DESC LIMIT 1";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare resolve calibration at");
  }
  sqlite3_bind_blob(stmt, 1, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, timestamp_ns);
  sqlite3_bind_int64(stmt, 3, timestamp_ns);
  std::optional<CalibrationRow> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    CalibrationRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.calibration_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.sensor_id = *u;
    row.version = sqlite3_column_int64(stmt, 2);
    row.calibration_time_ns = sqlite3_column_int64(stmt, 3);
    if (const auto* t = sqlite3_column_text(stmt, 4)) {
      row.source = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      row.intrinsics_json = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 6)) {
      row.distortion_json = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 7)) {
      row.extrinsics_json = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 8)) {
      row.uncertainty_json = reinterpret_cast<const char*>(t);
    }
    row.valid_from_ns = ColumnInt64OrNull(stmt, 9);
    row.valid_to_ns = ColumnInt64OrNull(stmt, 10);
    out = row;
  }
  sqlite3_finalize(stmt);
  return out;
}

std::optional<CaptureSessionRow> MetadataDb::FindCaptureSession(
    const Uuid& session_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT session_id, project_id, name, started_at_ns, ended_at_ns,"
      " source_uri, status, provenance_json FROM capture_sessions"
      " WHERE session_id = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find capture session");
  }
  sqlite3_bind_blob(stmt, 1, session_id.data(),
                    static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
  std::optional<CaptureSessionRow> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    CaptureSessionRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.session_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.project_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 2)) {
      row.name = reinterpret_cast<const char*>(t);
    }
    row.started_at_ns = sqlite3_column_int64(stmt, 3);
    row.ended_at_ns = sqlite3_column_int64(stmt, 4);
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      row.source_uri = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 6)) {
      row.status = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 7)) {
      row.provenance_json = reinterpret_cast<const char*>(t);
    }
    out = row;
  }
  sqlite3_finalize(stmt);
  return out;
}

std::optional<SceneRow> MetadataDb::FindSceneByProject(
    const Uuid& project_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT scene_id, current_version_id, name, origin_frame, crs, status,"
      " properties_json FROM scenes WHERE project_id = ? AND status = 'open'"
      " LIMIT 1";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find scene by project");
  }
  sqlite3_bind_blob(stmt, 1, project_id.data(),
                    static_cast<int>(project_id.size()), SQLITE_TRANSIENT);
  std::optional<SceneRow> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    SceneRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.scene_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.version_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 2)) {
      row.name = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 3)) {
      row.origin_frame = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 4)) {
      row.crs = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      row.status = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 6)) {
      row.properties_json = reinterpret_cast<const char*>(t);
    }
    row.project_id = project_id;
    out = row;
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<FrameRow> MetadataDb::FindFramesByScene(
    const Uuid& scene_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT frame_id, scene_id, session_id, timestamp_ns, sequence_index,"
      " sensor_id, pose_ref, properties_json FROM frames"
      " WHERE scene_id = ? ORDER BY timestamp_ns, sequence_index";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find frames by scene");
  }
  sqlite3_bind_blob(stmt, 1, scene_id.data(),
                    static_cast<int>(scene_id.size()), SQLITE_TRANSIENT);
  std::vector<FrameRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadFrameRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<FrameRow> MetadataDb::FindFramesBySession(
    const Uuid& session_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT frame_id, scene_id, session_id, timestamp_ns, sequence_index,"
      " sensor_id, pose_ref, properties_json FROM frames"
      " WHERE session_id = ? ORDER BY timestamp_ns, sequence_index";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find frames by session");
  }
  sqlite3_bind_blob(stmt, 1, session_id.data(),
                    static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
  std::vector<FrameRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadFrameRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<FrameRow> MetadataDb::FindFramesBySensor(
    const Uuid& sensor_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT frame_id, scene_id, session_id, timestamp_ns, sequence_index,"
      " sensor_id, pose_ref, properties_json FROM frames"
      " WHERE sensor_id = ? ORDER BY timestamp_ns, sequence_index";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find frames by sensor");
  }
  sqlite3_bind_blob(stmt, 1, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  std::vector<FrameRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadFrameRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<FrameRow> MetadataDb::FindFrames() const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT frame_id, scene_id, session_id, timestamp_ns, sequence_index,"
      " sensor_id, pose_ref, properties_json FROM frames"
      " ORDER BY timestamp_ns, sequence_index";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find all frames");
  }
  std::vector<FrameRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadFrameRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<FrameRow> MetadataDb::FindFramesInTimeRange(
    std::int64_t from_ns, std::int64_t to_ns) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT frame_id, scene_id, session_id, timestamp_ns, sequence_index,"
      " sensor_id, pose_ref, properties_json FROM frames"
      " WHERE timestamp_ns >= ? AND timestamp_ns < ?"
      " ORDER BY timestamp_ns, sequence_index";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find frames in time range");
  }
  sqlite3_bind_int64(stmt, 1, from_ns);
  sqlite3_bind_int64(stmt, 2, to_ns);
  std::vector<FrameRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadFrameRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ObservationRow> MetadataDb::FindObservationsByScene(
    const Uuid& scene_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT observation_id, scene_id, sensor_id, frame_id, session_id,"
      " timestamp_ns, type, artifact_ref, source_json, properties_json,"
      " width, height, pixel_format FROM observations"
      " LEFT JOIN observation_payloads USING (observation_id)"
      " WHERE scene_id = ? ORDER BY timestamp_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find observations by scene");
  }
  sqlite3_bind_blob(stmt, 1, scene_id.data(),
                    static_cast<int>(scene_id.size()), SQLITE_TRANSIENT);
  std::vector<ObservationRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadObservationRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ObservationRow> MetadataDb::FindObservationsByFrame(
    const Uuid& frame_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT observation_id, scene_id, sensor_id, frame_id, session_id,"
      " timestamp_ns, type, artifact_ref, source_json, properties_json,"
      " width, height, pixel_format FROM observations"
      " LEFT JOIN observation_payloads USING (observation_id)"
      " WHERE frame_id = ? ORDER BY timestamp_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find observations by frame");
  }
  sqlite3_bind_blob(stmt, 1, frame_id.data(),
                    static_cast<int>(frame_id.size()), SQLITE_TRANSIENT);
  std::vector<ObservationRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadObservationRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ObservationRow> MetadataDb::FindObservationsBySession(
    const Uuid& session_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT observation_id, scene_id, sensor_id, frame_id, session_id,"
      " timestamp_ns, type, artifact_ref, source_json, properties_json,"
      " width, height, pixel_format FROM observations"
      " LEFT JOIN observation_payloads USING (observation_id)"
      " WHERE session_id = ? ORDER BY timestamp_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find observations by session");
  }
  sqlite3_bind_blob(stmt, 1, session_id.data(),
                    static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
  std::vector<ObservationRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadObservationRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ObservationRow> MetadataDb::FindObservationsBySensor(
    const Uuid& sensor_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT observation_id, scene_id, sensor_id, frame_id, session_id,"
      " timestamp_ns, type, artifact_ref, source_json, properties_json,"
      " width, height, pixel_format FROM observations"
      " LEFT JOIN observation_payloads USING (observation_id)"
      " WHERE sensor_id = ? ORDER BY timestamp_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find observations by sensor");
  }
  sqlite3_bind_blob(stmt, 1, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  std::vector<ObservationRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadObservationRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ObservationRow> MetadataDb::FindObservations() const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT observation_id, scene_id, sensor_id, frame_id, session_id,"
      " timestamp_ns, type, artifact_ref, source_json, properties_json,"
      " width, height, pixel_format FROM observations"
      " LEFT JOIN observation_payloads USING (observation_id)"
      " ORDER BY timestamp_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find all observations");
  }
  std::vector<ObservationRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadObservationRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ObservationRow> MetadataDb::FindObservationsInTimeRange(
    std::int64_t from_ns, std::int64_t to_ns) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT observation_id, scene_id, sensor_id, frame_id, session_id,"
      " timestamp_ns, type, artifact_ref, source_json, properties_json,"
      " width, height, pixel_format FROM observations"
      " LEFT JOIN observation_payloads USING (observation_id)"
      " WHERE timestamp_ns >= ? AND timestamp_ns < ?"
      " ORDER BY timestamp_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find observations in time range");
  }
  sqlite3_bind_int64(stmt, 1, from_ns);
  sqlite3_bind_int64(stmt, 2, to_ns);
  std::vector<ObservationRow> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) out.push_back(ReadObservationRow(stmt));
  sqlite3_finalize(stmt);
  return out;
}

SceneRow MetadataDb::FindOrCreateScene(const Uuid& project_id,
                                       const std::string& name,
                                       const std::string& created_by_json,
                                       std::int64_t created_at_ns) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* find_sql =
      "SELECT scene_id, current_version_id, name, origin_frame, crs, status,"
      " properties_json FROM scenes WHERE project_id = ? AND status = 'open'"
      " LIMIT 1";
  if (sqlite3_prepare_v2(db_, find_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find scene");
  }
  sqlite3_bind_blob(stmt, 1, project_id.data(),
                    static_cast<int>(project_id.size()), SQLITE_TRANSIENT);
  SceneRow found;
  bool exists = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    exists = true;
    if (const auto u = ColumnUuid(stmt, 0)) found.scene_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) found.version_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 2)) {
      found.name = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 3)) {
      found.origin_frame = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 4)) {
      found.crs = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      found.status = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 6)) {
      found.properties_json = reinterpret_cast<const char*>(t);
    }
    found.project_id = project_id;
  }
  sqlite3_finalize(stmt);
  if (exists) return found;

  Exec("BEGIN IMMEDIATE TRANSACTION;", "begin create scene");
  try {
    const Uuid scene_id = GenerateUuid();
    const Uuid version_id = GenerateUuid();
    stmt = nullptr;
    const char* insert_sql =
        "INSERT INTO scenes (scene_id, project_id, schema_version, name,"
        " current_version_id, origin_frame, crs, status, properties_json)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare insert scene");
    }
    sqlite3_bind_blob(stmt, 1, scene_id.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, project_id.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, 1);
    sqlite3_bind_text(stmt, 4, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 5, version_id.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_null(stmt, 6);
    sqlite3_bind_null(stmt, 7);
    sqlite3_bind_text(stmt, 8, "open", -1, SQLITE_TRANSIENT);
    sqlite3_bind_null(stmt, 9);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      const std::string msg = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot insert scene row: " + msg);
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    const char* version_sql =
        "INSERT INTO scene_versions (version_id, scene_id, parent_version_id,"
        " stage, created_by_json, created_at_ns, status)"
        " VALUES (?, ?, ?, 'created', ?, ?, 'active')";
    if (sqlite3_prepare_v2(db_, version_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare insert scene version");
    }
    sqlite3_bind_blob(stmt, 1, version_id.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, scene_id.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_null(stmt, 3);
    sqlite3_bind_text(stmt, 4, created_by_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, created_at_ns);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      const std::string msg = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot insert scene version row: " + msg);
    }
    sqlite3_finalize(stmt);
    Exec("COMMIT;", "commit create scene");
    found.scene_id = scene_id;
    found.version_id = version_id;
    found.project_id = project_id;
    found.name = name;
    found.schema_version = 1;
    found.stage = "created";
    found.created_by_json = created_by_json;
    found.created_at_ns = created_at_ns;
    found.status = "open";
  } catch (...) {
    Exec("ROLLBACK;", "rollback create scene");
    throw;
  }
  return found;
}

SceneVersionRow MetadataDb::CreateSceneVersion(const Uuid& scene_id,
                                               const std::string& stage,
                                               const std::string& created_by_json,
                                               std::int64_t created_at_ns) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  Exec("BEGIN IMMEDIATE TRANSACTION;", "begin create scene version");
  SceneVersionRow out;
  try {
    sqlite3_stmt* stmt = nullptr;
    const char* parent_sql =
        "SELECT current_version_id FROM scenes WHERE scene_id = ?";
    if (sqlite3_prepare_v2(db_, parent_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare read current scene version");
    }
    sqlite3_bind_blob(stmt, 1, scene_id.data(), 16, SQLITE_TRANSIENT);
    Uuid parent{};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      if (const auto u = ColumnUuid(stmt, 0)) parent = *u;
    }
    sqlite3_finalize(stmt);
    if (IsNil(parent)) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "scene has no current version: " + FormatUuid(scene_id));
    }

    const Uuid version_id = GenerateUuid();
    stmt = nullptr;
    const char* version_sql =
        "INSERT INTO scene_versions (version_id, scene_id, parent_version_id,"
        " stage, created_by_json, created_at_ns, status)"
        " VALUES (?, ?, ?, ?, ?, ?, 'active')";
    if (sqlite3_prepare_v2(db_, version_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare insert scene version");
    }
    sqlite3_bind_blob(stmt, 1, version_id.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, scene_id.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, parent.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, stage.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, created_by_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, created_at_ns);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      const std::string msg = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot insert scene version row: " + msg);
    }
    sqlite3_finalize(stmt);

    stmt = nullptr;
    const char* update_sql =
        "UPDATE scenes SET current_version_id = ? WHERE scene_id = ?";
    if (sqlite3_prepare_v2(db_, update_sql, -1, &stmt, nullptr) != SQLITE_OK) {
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot prepare advance scene version");
    }
    sqlite3_bind_blob(stmt, 1, version_id.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 2, scene_id.data(), 16, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      const std::string msg = sqlite3_errmsg(db_);
      sqlite3_finalize(stmt);
      throw SchemaError(ErrorCode::kSchemaInvalid,
                        "cannot advance scene version: " + msg);
    }
    sqlite3_finalize(stmt);
    Exec("COMMIT;", "commit create scene version");

    out.version_id = version_id;
    out.scene_id = scene_id;
    out.parent_version_id = parent;
    out.stage = stage;
    out.created_by_json = created_by_json;
    out.created_at_ns = created_at_ns;
    out.status = "active";
  } catch (...) {
    Exec("ROLLBACK;", "rollback create scene version");
    throw;
  }
  return out;
}

std::optional<SceneVersionRow> MetadataDb::FindSceneVersion(
    const Uuid& version_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT version_id, scene_id, parent_version_id, stage, created_by_json,"
      " created_at_ns, status FROM scene_versions WHERE version_id = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find scene version");
  }
  sqlite3_bind_blob(stmt, 1, version_id.data(),
                    static_cast<int>(version_id.size()), SQLITE_TRANSIENT);
  std::optional<SceneVersionRow> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out = ReadSceneVersionRow(stmt);
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<SceneVersionRow> MetadataDb::FindSceneVersionsByScene(
    const Uuid& scene_id) const {
  std::vector<SceneVersionRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT version_id, scene_id, parent_version_id, stage, created_by_json,"
      " created_at_ns, status FROM scene_versions WHERE scene_id = ?"
      " ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find scene versions");
  }
  sqlite3_bind_blob(stmt, 1, scene_id.data(),
                    static_cast<int>(scene_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(ReadSceneVersionRow(stmt));
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::InsertFeatureSet(const FeatureSetRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO feature_sets (feature_set_id, frame_id, detector,"
      " descriptor_type, count, artifact_ref) VALUES (?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert feature set");
  }
  sqlite3_bind_blob(stmt, 1, row.feature_set_id.data(),
                    static_cast<int>(row.feature_set_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.frame_id.data(),
                    static_cast<int>(row.frame_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, row.detector.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, row.descriptor_type.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, row.count);
  sqlite3_bind_text(stmt, 6, row.artifact_ref.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert feature set row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::vector<FeatureSetRow> MetadataDb::FindFeatureSetsByFrame(
    const Uuid& frame_id) const {
  std::vector<FeatureSetRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT feature_set_id, frame_id, detector, descriptor_type, count,"
      " artifact_ref FROM feature_sets WHERE frame_id = ?"
      " ORDER BY detector, descriptor_type";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find feature sets by frame");
  }
  sqlite3_bind_blob(stmt, 1, frame_id.data(),
                    static_cast<int>(frame_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(ReadFeatureSetRow(stmt));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<FeatureSetRow> MetadataDb::FindFeatureSetsByScene(
    const Uuid& scene_id) const {
  std::vector<FeatureSetRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT fs.feature_set_id, fs.frame_id, fs.detector, fs.descriptor_type,"
      " fs.count, fs.artifact_ref FROM feature_sets fs"
      " JOIN frames f ON f.frame_id = fs.frame_id WHERE f.scene_id = ?"
      " ORDER BY fs.detector, fs.descriptor_type";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find feature sets by scene");
  }
  sqlite3_bind_blob(stmt, 1, scene_id.data(),
                    static_cast<int>(scene_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(ReadFeatureSetRow(stmt));
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::InsertFrame(const FrameRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO frames (frame_id, scene_id, session_id, timestamp_ns,"
      " sequence_index, sensor_id, pose_ref, properties_json)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert frame");
  }
  sqlite3_bind_blob(stmt, 1, row.frame_id.data(),
                    static_cast<int>(row.frame_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.scene_id.data(),
                    static_cast<int>(row.scene_id.size()), SQLITE_TRANSIENT);
  BindUuidOrNull(stmt, 3, row.session_id);
  sqlite3_bind_int64(stmt, 4, row.timestamp_ns);
  sqlite3_bind_int64(stmt, 5, row.sequence_index);
  BindUuidOrNull(stmt, 6, row.sensor_id);
  BindUuidOrNull(stmt, 7, row.pose_ref);
  sqlite3_bind_text(stmt, 8, row.properties_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert frame row: " + msg);
  }
  sqlite3_finalize(stmt);
}

void MetadataDb::InsertObservation(const ObservationRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO observations (observation_id, scene_id, sensor_id, frame_id,"
      " session_id, timestamp_ns, type, artifact_ref, source_json,"
      " properties_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert observation");
  }
  sqlite3_bind_blob(stmt, 1, row.observation_id.data(),
                    static_cast<int>(row.observation_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.scene_id.data(),
                    static_cast<int>(row.scene_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, row.sensor_id.data(),
                    static_cast<int>(row.sensor_id.size()), SQLITE_TRANSIENT);
  BindUuidOrNull(stmt, 4, row.frame_id);
  BindUuidOrNull(stmt, 5, row.session_id);
  sqlite3_bind_int64(stmt, 6, row.timestamp_ns);
  sqlite3_bind_text(stmt, 7, row.type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, row.artifact_ref.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, row.source_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, row.properties_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert observation row: " + msg);
  }
  sqlite3_finalize(stmt);
}

void MetadataDb::InsertObservationPayload(
    const ObservationPayloadRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO observation_payloads (observation_id, width, height,"
      " pixel_format) VALUES (?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert observation payload");
  }
  sqlite3_bind_blob(stmt, 1, row.observation_id.data(),
                    static_cast<int>(row.observation_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, row.width);
  sqlite3_bind_int64(stmt, 3, row.height);
  sqlite3_bind_text(stmt, 4, row.pixel_format.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert observation payload row: " + msg);
  }
  sqlite3_finalize(stmt);
}

bool MetadataDb::FrameExists(const Uuid& frame_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT 1 FROM frames WHERE frame_id = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find frame");
  }
  sqlite3_bind_blob(stmt, 1, frame_id.data(),
                    static_cast<int>(frame_id.size()), SQLITE_TRANSIENT);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
}

bool MetadataDb::ObservationExists(const Uuid& observation_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT 1 FROM observations WHERE observation_id = ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find observation");
  }
  sqlite3_bind_blob(stmt, 1, observation_id.data(),
                    static_cast<int>(observation_id.size()), SQLITE_TRANSIENT);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
}

void MetadataDb::InsertImportRejection(const ImportRejectionRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO import_rejections (rejection_id, project_id, session_id,"
      " sequence_index, source_path, mime_type, importer, importer_version,"
      " error_code, diagnostic, rejected_at_ns)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert import rejection");
  }
  sqlite3_bind_blob(stmt, 1, row.rejection_id.data(),
                    static_cast<int>(row.rejection_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.project_id.data(),
                    static_cast<int>(row.project_id.size()), SQLITE_TRANSIENT);
  BindUuidOrNull(stmt, 3, row.session_id);
  sqlite3_bind_int64(stmt, 4, row.sequence_index);
  sqlite3_bind_text(stmt, 5, row.source_path.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, row.mime_type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, row.importer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, row.importer_version.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, row.error_code.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, row.diagnostic.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 11, row.rejected_at_ns);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert import rejection row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::vector<ImportRejectionRow> MetadataDb::FindImportRejectionsBySession(
    const Uuid& session_id) const {
  std::vector<ImportRejectionRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT rejection_id, project_id, session_id, sequence_index,"
      " source_path, mime_type, importer, importer_version, error_code,"
      " diagnostic, rejected_at_ns FROM import_rejections"
      " WHERE session_id = ? ORDER BY sequence_index, rejected_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find import rejections");
  }
  sqlite3_bind_blob(stmt, 1, session_id.data(),
                    static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ImportRejectionRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.rejection_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.project_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.session_id = *u;
    row.sequence_index = sqlite3_column_int64(stmt, 3);
    if (const auto* t = sqlite3_column_text(stmt, 4)) {
      row.source_path = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      row.mime_type = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 6)) {
      row.importer = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 7)) {
      row.importer_version = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 8)) {
      row.error_code = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 9)) {
      row.diagnostic = reinterpret_cast<const char*>(t);
    }
    row.rejected_at_ns = sqlite3_column_int64(stmt, 10);
    out.push_back(row);
  }
  sqlite3_finalize(stmt);
  return out;
}

// --- P2.5 reconstruction (migration 0007, D-CRM-15) ---

void MetadataDb::AddReconstruction(const ReconstructionRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO reconstructions (reconstruction_id, scene_id,"
      " coordinate_frame, status, created_at_ns, document_json)"
      " VALUES (?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert reconstruction");
  }
  sqlite3_bind_blob(stmt, 1, row.reconstruction_id.data(),
                    static_cast<int>(row.reconstruction_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.scene_id.data(),
                    static_cast<int>(row.scene_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, row.coordinate_frame.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, row.status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, row.created_at_ns);
  sqlite3_bind_text(stmt, 6, row.document_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert reconstruction row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::optional<ReconstructionRow> MetadataDb::QueryLatestReconstructionByScene(
    const Uuid& scene_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT reconstruction_id, scene_id, coordinate_frame, status,"
      " created_at_ns, document_json FROM reconstructions"
      " WHERE scene_id = ? AND status = 'succeeded'"
      " ORDER BY created_at_ns DESC LIMIT 1";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare query latest reconstruction");
  }
  sqlite3_bind_blob(stmt, 1, scene_id.data(),
                    static_cast<int>(scene_id.size()), SQLITE_TRANSIENT);
  std::optional<ReconstructionRow> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    ReconstructionRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.reconstruction_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.scene_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 2)) {
      row.coordinate_frame = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 3)) {
      row.status = reinterpret_cast<const char*>(t);
    }
    row.created_at_ns = sqlite3_column_int64(stmt, 4);
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      row.document_json = reinterpret_cast<const char*>(t);
    }
    result = std::move(row);
  }
  sqlite3_finalize(stmt);
  return result;
}

std::vector<ReconstructionRow> MetadataDb::FindReconstructionsByScene(
    const Uuid& scene_id) const {
  std::vector<ReconstructionRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT reconstruction_id, scene_id, coordinate_frame, status,"
      " created_at_ns, document_json FROM reconstructions"
      " WHERE scene_id = ? ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find reconstructions by scene");
  }
  sqlite3_bind_blob(stmt, 1, scene_id.data(),
                    static_cast<int>(scene_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    ReconstructionRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.reconstruction_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.scene_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 2)) {
      row.coordinate_frame = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 3)) {
      row.status = reinterpret_cast<const char*>(t);
    }
    row.created_at_ns = sqlite3_column_int64(stmt, 4);
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      row.document_json = reinterpret_cast<const char*>(t);
    }
    out.push_back(row);
  }
  sqlite3_finalize(stmt);
  return out;
}

// --- P3-impl-1 trajectory / pose graph / loop closure / optimization (migration 0008) ---

void MetadataDb::AddTrajectory(const TrajectoryRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO trajectories (trajectory_id, scene_id, session_id,"
      " kind, coordinate_frame, status, node_count, total_distance_m,"
      " total_duration_ns, created_at_ns, document_json)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert trajectory");
  }
  sqlite3_bind_blob(stmt, 1, row.trajectory_id.data(),
                    static_cast<int>(row.trajectory_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.scene_id.data(),
                    static_cast<int>(row.scene_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, row.session_id.data(),
                    static_cast<int>(row.session_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, row.kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, row.coordinate_frame.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, row.status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, row.node_count);
  sqlite3_bind_double(stmt, 8, row.total_distance_m);
  sqlite3_bind_int64(stmt, 9, row.total_duration_ns);
  sqlite3_bind_int64(stmt, 10, row.created_at_ns);
  sqlite3_bind_text(stmt, 11, row.document_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert trajectory row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::optional<TrajectoryRow> MetadataDb::QueryLatestTrajectoryBySession(
    const Uuid& session_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT trajectory_id, scene_id, session_id, kind, coordinate_frame,"
      " status, node_count, total_distance_m, total_duration_ns,"
      " created_at_ns, document_json FROM trajectories"
      " WHERE session_id = ? AND status != 'superseded'"
      " ORDER BY created_at_ns DESC LIMIT 1";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare query latest trajectory");
  }
  sqlite3_bind_blob(stmt, 1, session_id.data(),
                    static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
  std::optional<TrajectoryRow> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    TrajectoryRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.trajectory_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.scene_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.session_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 3))
      row.kind = reinterpret_cast<const char*>(t);
    if (const auto* t = sqlite3_column_text(stmt, 4))
      row.coordinate_frame = reinterpret_cast<const char*>(t);
    if (const auto* t = sqlite3_column_text(stmt, 5))
      row.status = reinterpret_cast<const char*>(t);
    row.node_count = sqlite3_column_int64(stmt, 6);
    row.total_distance_m = sqlite3_column_double(stmt, 7);
    row.total_duration_ns = sqlite3_column_int64(stmt, 8);
    row.created_at_ns = sqlite3_column_int64(stmt, 9);
    if (const auto* t = sqlite3_column_text(stmt, 10))
      row.document_json = reinterpret_cast<const char*>(t);
    result = std::move(row);
  }
  sqlite3_finalize(stmt);
  return result;
}

std::vector<TrajectoryRow> MetadataDb::FindTrajectoriesByScene(
    const Uuid& scene_id) const {
  std::vector<TrajectoryRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT trajectory_id, scene_id, session_id, kind, coordinate_frame,"
      " status, node_count, total_distance_m, total_duration_ns,"
      " created_at_ns, document_json FROM trajectories"
      " WHERE scene_id = ? ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find trajectories by scene");
  }
  sqlite3_bind_blob(stmt, 1, scene_id.data(),
                    static_cast<int>(scene_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    TrajectoryRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.trajectory_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.scene_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.session_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 3))
      row.kind = reinterpret_cast<const char*>(t);
    if (const auto* t = sqlite3_column_text(stmt, 4))
      row.coordinate_frame = reinterpret_cast<const char*>(t);
    if (const auto* t = sqlite3_column_text(stmt, 5))
      row.status = reinterpret_cast<const char*>(t);
    row.node_count = sqlite3_column_int64(stmt, 6);
    row.total_distance_m = sqlite3_column_double(stmt, 7);
    row.total_duration_ns = sqlite3_column_int64(stmt, 8);
    row.created_at_ns = sqlite3_column_int64(stmt, 9);
    if (const auto* t = sqlite3_column_text(stmt, 10))
      row.document_json = reinterpret_cast<const char*>(t);
    out.push_back(row);
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<TrajectoryRow> MetadataDb::FindTrajectoriesBySession(
    const Uuid& session_id) const {
  std::vector<TrajectoryRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT trajectory_id, scene_id, session_id, kind, coordinate_frame,"
      " status, node_count, total_distance_m, total_duration_ns,"
      " created_at_ns, document_json FROM trajectories"
      " WHERE session_id = ? ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find trajectories by session");
  }
  sqlite3_bind_blob(stmt, 1, session_id.data(),
                    static_cast<int>(session_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    TrajectoryRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.trajectory_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.scene_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.session_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 3))
      row.kind = reinterpret_cast<const char*>(t);
    if (const auto* t = sqlite3_column_text(stmt, 4))
      row.coordinate_frame = reinterpret_cast<const char*>(t);
    if (const auto* t = sqlite3_column_text(stmt, 5))
      row.status = reinterpret_cast<const char*>(t);
    row.node_count = sqlite3_column_int64(stmt, 6);
    row.total_distance_m = sqlite3_column_double(stmt, 7);
    row.total_duration_ns = sqlite3_column_int64(stmt, 8);
    row.created_at_ns = sqlite3_column_int64(stmt, 9);
    if (const auto* t = sqlite3_column_text(stmt, 10))
      row.document_json = reinterpret_cast<const char*>(t);
    out.push_back(row);
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::AddPoseGraph(const PoseGraphRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO pose_graphs (graph_id, trajectory_id, scene_id,"
      " status, node_count, edge_count, odometry_edge_count,"
      " loop_closure_edge_count, prior_edge_count, created_at_ns,"
      " document_json) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert pose graph");
  }
  sqlite3_bind_blob(stmt, 1, row.graph_id.data(),
                    static_cast<int>(row.graph_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.trajectory_id.data(),
                    static_cast<int>(row.trajectory_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, row.scene_id.data(),
                    static_cast<int>(row.scene_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, row.status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, row.node_count);
  sqlite3_bind_int64(stmt, 6, row.edge_count);
  sqlite3_bind_int64(stmt, 7, row.odometry_edge_count);
  sqlite3_bind_int64(stmt, 8, row.loop_closure_edge_count);
  sqlite3_bind_int64(stmt, 9, row.prior_edge_count);
  sqlite3_bind_int64(stmt, 10, row.created_at_ns);
  sqlite3_bind_text(stmt, 11, row.document_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert pose graph row: " + msg);
  }
  sqlite3_finalize(stmt);
}

namespace {

PoseGraphRow ReadPoseGraphRow(sqlite3_stmt* stmt) {
  PoseGraphRow row;
  if (const auto u = ColumnUuid(stmt, 0)) row.graph_id = *u;
  if (const auto u = ColumnUuid(stmt, 1)) row.trajectory_id = *u;
  if (const auto u = ColumnUuid(stmt, 2)) row.scene_id = *u;
  if (const auto* t = sqlite3_column_text(stmt, 3))
    row.status = reinterpret_cast<const char*>(t);
  row.node_count = sqlite3_column_int64(stmt, 4);
  row.edge_count = sqlite3_column_int64(stmt, 5);
  row.odometry_edge_count = sqlite3_column_int64(stmt, 6);
  row.loop_closure_edge_count = sqlite3_column_int64(stmt, 7);
  row.prior_edge_count = sqlite3_column_int64(stmt, 8);
  row.created_at_ns = sqlite3_column_int64(stmt, 9);
  if (const auto* t = sqlite3_column_text(stmt, 10))
    row.document_json = reinterpret_cast<const char*>(t);
  return row;
}

}  // namespace

std::optional<PoseGraphRow> MetadataDb::QueryLatestPoseGraphByTrajectory(
    const Uuid& trajectory_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT graph_id, trajectory_id, scene_id, status, node_count,"
      " edge_count, odometry_edge_count, loop_closure_edge_count,"
      " prior_edge_count, created_at_ns, document_json FROM pose_graphs"
      " WHERE trajectory_id = ? ORDER BY created_at_ns DESC LIMIT 1";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare query latest pose graph");
  }
  sqlite3_bind_blob(stmt, 1, trajectory_id.data(),
                    static_cast<int>(trajectory_id.size()), SQLITE_TRANSIENT);
  std::optional<PoseGraphRow> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    result = ReadPoseGraphRow(stmt);
  }
  sqlite3_finalize(stmt);
  return result;
}

std::vector<PoseGraphRow> MetadataDb::FindPoseGraphsByTrajectory(
    const Uuid& trajectory_id) const {
  std::vector<PoseGraphRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT graph_id, trajectory_id, scene_id, status, node_count,"
      " edge_count, odometry_edge_count, loop_closure_edge_count,"
      " prior_edge_count, created_at_ns, document_json FROM pose_graphs"
      " WHERE trajectory_id = ? ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find pose graphs by trajectory");
  }
  sqlite3_bind_blob(stmt, 1, trajectory_id.data(),
                    static_cast<int>(trajectory_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(ReadPoseGraphRow(stmt));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<PoseGraphRow> MetadataDb::FindPoseGraphsByScene(
    const Uuid& scene_id) const {
  std::vector<PoseGraphRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT graph_id, trajectory_id, scene_id, status, node_count,"
      " edge_count, odometry_edge_count, loop_closure_edge_count,"
      " prior_edge_count, created_at_ns, document_json FROM pose_graphs"
      " WHERE scene_id = ? ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find pose graphs by scene");
  }
  sqlite3_bind_blob(stmt, 1, scene_id.data(),
                    static_cast<int>(scene_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(ReadPoseGraphRow(stmt));
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::AddLoopClosureCandidate(const LoopClosureCandidateRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO loop_closure_candidates (candidate_id, trajectory_id,"
      " source_frame_id, target_frame_id, feature_match_score, matcher,"
      " created_at_ns) VALUES (?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert loop closure candidate");
  }
  sqlite3_bind_blob(stmt, 1, row.candidate_id.data(),
                    static_cast<int>(row.candidate_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.trajectory_id.data(),
                    static_cast<int>(row.trajectory_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, row.source_frame_id.data(),
                    static_cast<int>(row.source_frame_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 4, row.target_frame_id.data(),
                    static_cast<int>(row.target_frame_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 5, row.feature_match_score);
  sqlite3_bind_text(stmt, 6, row.matcher.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, row.created_at_ns);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert loop closure candidate row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::vector<LoopClosureCandidateRow>
MetadataDb::FindLoopClosureCandidatesByTrajectory(
    const Uuid& trajectory_id) const {
  std::vector<LoopClosureCandidateRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT candidate_id, trajectory_id, source_frame_id, target_frame_id,"
      " feature_match_score, matcher, created_at_ns"
      " FROM loop_closure_candidates WHERE trajectory_id = ?"
      " ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find loop closure candidates");
  }
  sqlite3_bind_blob(stmt, 1, trajectory_id.data(),
                    static_cast<int>(trajectory_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    LoopClosureCandidateRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.candidate_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.trajectory_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.source_frame_id = *u;
    if (const auto u = ColumnUuid(stmt, 3)) row.target_frame_id = *u;
    row.feature_match_score = sqlite3_column_double(stmt, 4);
    if (const auto* t = sqlite3_column_text(stmt, 5))
      row.matcher = reinterpret_cast<const char*>(t);
    row.created_at_ns = sqlite3_column_int64(stmt, 6);
    out.push_back(row);
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::AddLoopClosure(const LoopClosureRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO loop_closures (closure_id, trajectory_id, candidate_id,"
      " source_frame_id, target_frame_id, status, inlier_ratio,"
      " inlier_count, confidence, temporal_separation_ns,"
      " spatial_separation_m, created_at_ns)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert loop closure");
  }
  sqlite3_bind_blob(stmt, 1, row.closure_id.data(),
                    static_cast<int>(row.closure_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.trajectory_id.data(),
                    static_cast<int>(row.trajectory_id.size()),
                    SQLITE_TRANSIENT);
  BindUuidOrNull(stmt, 3, row.candidate_id);
  sqlite3_bind_blob(stmt, 4, row.source_frame_id.data(),
                    static_cast<int>(row.source_frame_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 5, row.target_frame_id.data(),
                    static_cast<int>(row.target_frame_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, row.status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 7, row.inlier_ratio);
  sqlite3_bind_int64(stmt, 8, row.inlier_count);
  sqlite3_bind_double(stmt, 9, row.confidence);
  sqlite3_bind_int64(stmt, 10, row.temporal_separation_ns);
  sqlite3_bind_double(stmt, 11, row.spatial_separation_m);
  sqlite3_bind_int64(stmt, 12, row.created_at_ns);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert loop closure row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::vector<LoopClosureRow> MetadataDb::FindLoopClosuresByTrajectory(
    const Uuid& trajectory_id) const {
  std::vector<LoopClosureRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT closure_id, trajectory_id, candidate_id, source_frame_id,"
      " target_frame_id, status, inlier_ratio, inlier_count, confidence,"
      " temporal_separation_ns, spatial_separation_m, created_at_ns"
      " FROM loop_closures WHERE trajectory_id = ?"
      " ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find loop closures");
  }
  sqlite3_bind_blob(stmt, 1, trajectory_id.data(),
                    static_cast<int>(trajectory_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    LoopClosureRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.closure_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.trajectory_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.candidate_id = *u;
    if (const auto u = ColumnUuid(stmt, 3)) row.source_frame_id = *u;
    if (const auto u = ColumnUuid(stmt, 4)) row.target_frame_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 5))
      row.status = reinterpret_cast<const char*>(t);
    row.inlier_ratio = sqlite3_column_double(stmt, 6);
    row.inlier_count = sqlite3_column_int64(stmt, 7);
    row.confidence = sqlite3_column_double(stmt, 8);
    row.temporal_separation_ns = sqlite3_column_int64(stmt, 9);
    row.spatial_separation_m = sqlite3_column_double(stmt, 10);
    row.created_at_ns = sqlite3_column_int64(stmt, 11);
    out.push_back(row);
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<LoopClosureRow> MetadataDb::FindAcceptedLoopClosuresByTrajectory(
    const Uuid& trajectory_id) const {
  std::vector<LoopClosureRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT closure_id, trajectory_id, candidate_id, source_frame_id,"
      " target_frame_id, status, inlier_ratio, inlier_count, confidence,"
      " temporal_separation_ns, spatial_separation_m, created_at_ns"
      " FROM loop_closures WHERE trajectory_id = ? AND status = 'accepted'"
      " ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find accepted loop closures");
  }
  sqlite3_bind_blob(stmt, 1, trajectory_id.data(),
                    static_cast<int>(trajectory_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    LoopClosureRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.closure_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.trajectory_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.candidate_id = *u;
    if (const auto u = ColumnUuid(stmt, 3)) row.source_frame_id = *u;
    if (const auto u = ColumnUuid(stmt, 4)) row.target_frame_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 5))
      row.status = reinterpret_cast<const char*>(t);
    row.inlier_ratio = sqlite3_column_double(stmt, 6);
    row.inlier_count = sqlite3_column_int64(stmt, 7);
    row.confidence = sqlite3_column_double(stmt, 8);
    row.temporal_separation_ns = sqlite3_column_int64(stmt, 9);
    row.spatial_separation_m = sqlite3_column_double(stmt, 10);
    row.created_at_ns = sqlite3_column_int64(stmt, 11);
    out.push_back(row);
  }
  sqlite3_finalize(stmt);
  return out;
}

void MetadataDb::AddOptimizationResult(const OptimizationResultRow& row) {
  if (read_only_) {
    throw StorageError(ErrorCode::kStorageReadOnly,
                       "cannot write to a read-only project", {}, false,
                       "Open the project for writing to modify it.");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO optimization_results (result_id, graph_id, trajectory_id,"
      " status, iterations, initial_error, final_error, error_reduction,"
      " created_at_ns, document_json)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare insert optimization result");
  }
  sqlite3_bind_blob(stmt, 1, row.result_id.data(),
                    static_cast<int>(row.result_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, row.graph_id.data(),
                    static_cast<int>(row.graph_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, row.trajectory_id.data(),
                    static_cast<int>(row.trajectory_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, row.status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, row.iterations);
  sqlite3_bind_double(stmt, 6, row.initial_error);
  sqlite3_bind_double(stmt, 7, row.final_error);
  sqlite3_bind_double(stmt, 8, row.error_reduction);
  sqlite3_bind_int64(stmt, 9, row.created_at_ns);
  sqlite3_bind_text(stmt, 10, row.document_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string msg = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot insert optimization result row: " + msg);
  }
  sqlite3_finalize(stmt);
}

std::optional<OptimizationResultRow>
MetadataDb::QueryLatestOptimizationResultByTrajectory(
    const Uuid& trajectory_id) const {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT result_id, graph_id, trajectory_id, status, iterations,"
      " initial_error, final_error, error_reduction, created_at_ns,"
      " document_json FROM optimization_results"
      " WHERE trajectory_id = ? ORDER BY created_at_ns DESC LIMIT 1";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare query latest optimization result");
  }
  sqlite3_bind_blob(stmt, 1, trajectory_id.data(),
                    static_cast<int>(trajectory_id.size()), SQLITE_TRANSIENT);
  std::optional<OptimizationResultRow> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    OptimizationResultRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.result_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.graph_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.trajectory_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 3))
      row.status = reinterpret_cast<const char*>(t);
    row.iterations = sqlite3_column_int64(stmt, 4);
    row.initial_error = sqlite3_column_double(stmt, 5);
    row.final_error = sqlite3_column_double(stmt, 6);
    row.error_reduction = sqlite3_column_double(stmt, 7);
    row.created_at_ns = sqlite3_column_int64(stmt, 8);
    if (const auto* t = sqlite3_column_text(stmt, 9))
      row.document_json = reinterpret_cast<const char*>(t);
    result = std::move(row);
  }
  sqlite3_finalize(stmt);
  return result;
}

std::vector<OptimizationResultRow>
MetadataDb::FindOptimizationResultsByTrajectory(
    const Uuid& trajectory_id) const {
  std::vector<OptimizationResultRow> out;
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT result_id, graph_id, trajectory_id, status, iterations,"
      " initial_error, final_error, error_reduction, created_at_ns,"
      " document_json FROM optimization_results"
      " WHERE trajectory_id = ? ORDER BY created_at_ns";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw SchemaError(ErrorCode::kSchemaInvalid,
                      "cannot prepare find optimization results");
  }
  sqlite3_bind_blob(stmt, 1, trajectory_id.data(),
                    static_cast<int>(trajectory_id.size()), SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    OptimizationResultRow row;
    if (const auto u = ColumnUuid(stmt, 0)) row.result_id = *u;
    if (const auto u = ColumnUuid(stmt, 1)) row.graph_id = *u;
    if (const auto u = ColumnUuid(stmt, 2)) row.trajectory_id = *u;
    if (const auto* t = sqlite3_column_text(stmt, 3))
      row.status = reinterpret_cast<const char*>(t);
    row.iterations = sqlite3_column_int64(stmt, 4);
    row.initial_error = sqlite3_column_double(stmt, 5);
    row.final_error = sqlite3_column_double(stmt, 6);
    row.error_reduction = sqlite3_column_double(stmt, 7);
    row.created_at_ns = sqlite3_column_int64(stmt, 8);
    if (const auto* t = sqlite3_column_text(stmt, 9))
      row.document_json = reinterpret_cast<const char*>(t);
    out.push_back(row);
  }
  sqlite3_finalize(stmt);
  return out;
}

}  // namespace spatial::core
