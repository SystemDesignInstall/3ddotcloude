#include "core/storage/metadata_db.h"

#include <algorithm>
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
    ArtifactIndexRow row;
    const auto* blob = sqlite3_column_blob(stmt, 0);
    const int size = sqlite3_column_bytes(stmt, 0);
    if (blob && size == 16) {
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
    out = row;
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
    ArtifactIndexRow row;
    const auto* blob = sqlite3_column_blob(stmt, 0);
    const int size = sqlite3_column_bytes(stmt, 0);
    if (blob && size == 16) {
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
    out.push_back(row);
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

std::optional<SensorRow> MetadataDb::FindSensor(const Uuid& sensor_id) {
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

}  // namespace spatial::core
