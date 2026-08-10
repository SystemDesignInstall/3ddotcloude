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
  Uuid artifact_id{};
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

// Scene write records (RFC-0006 §6.7). Mirrors the migration-0001 tables;
// typed accessors land with the P2.1 importer. All UUIDs serialize as 16-byte
// BLOBs; nil UUID columns bind as NULL.
struct CaptureSessionRow {
  Uuid session_id{};
  Uuid project_id{};
  std::string name;
  std::int64_t started_at_ns = 0;
  std::int64_t ended_at_ns = 0;
  std::string source_uri;
  std::string status = "open";
  std::string provenance_json;
};

struct FrameRow {
  Uuid frame_id{};
  Uuid scene_id{};
  Uuid session_id{};
  std::int64_t timestamp_ns = 0;
  std::int64_t sequence_index = 0;
  Uuid sensor_id{};
  Uuid pose_ref{};  // nil UUID == no pose at import
  std::string properties_json;
};

struct ObservationRow {
  Uuid observation_id{};
  Uuid scene_id{};
  Uuid sensor_id{};
  Uuid frame_id{};
  Uuid session_id{};
  std::int64_t timestamp_ns = 0;
  std::string type;  // image | lidar | imu | gnss | depth | panoramic
  // Canonical UUID string of the ImageArtifact (observation.artifact_ref is a
  // TEXT column, 0001_init.sql:148; RFC-0006 §6.7).
  std::string artifact_ref;
  std::string source_json;
  std::string properties_json;
  // Image payload fields from observation_payloads (0001_init.sql:153),
  // populated by the LEFT JOINed read accessors. Zero/empty when no payload
  // row exists (non-image observation types).
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::string pixel_format;
};

struct ObservationPayloadRow {
  Uuid observation_id{};
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::string pixel_format;
};

// Persistent provenance of a rejected import input (RFC-0006 §14, migration
// 0005). A rejected file never creates an artifact/frame/observation; the
// rejection record keeps the run auditable: original path, detected MIME,
// importer identity, stable IMPORT_* error code, and the timestamp.
struct ImportRejectionRow {
  Uuid rejection_id{};
  Uuid project_id{};
  Uuid session_id{};  // the batch session; always set (batch never spans)
  std::int64_t sequence_index = 0;
  std::string source_path;
  std::string mime_type;
  std::string importer;
  std::string importer_version;
  std::string error_code;
  std::string diagnostic;
  std::int64_t rejected_at_ns = 0;
};

struct SceneRow {
  Uuid scene_id{};
  std::int64_t schema_version = 1;
  Uuid project_id{};
  std::string name;
  Uuid version_id{};         // current version (ADR-033)
  Uuid parent_version_id{};
  std::string stage = "created";
  std::string created_by_json;
  std::int64_t created_at_ns = 0;
  std::string origin_frame;
  std::string crs;
  std::string status = "open";
  std::string properties_json;
};

struct SceneVersionRow {
  Uuid version_id{};
  Uuid scene_id{};
  Uuid parent_version_id{};
  std::string stage;
  std::string created_by_json;
  std::int64_t created_at_ns = 0;
  std::string status = "active";
};

struct SensorRow {
  Uuid sensor_id{};
  Uuid project_id{};
  std::string type;          // camera | lidar | imu | gnss | rgbd | ...
  std::string manufacturer;
  std::string model;
  std::string serial_number;
  std::string time_domain;
  Uuid calibration_id{};     // nil UUID == none registered
  Uuid rig_id{};             // nil UUID == not rigged
  std::string source_json;
  std::string status;
  // True when the sensor has at least one calibrations row (migration 0001
  // carries no validity-interval columns; see FindSensor).
  bool has_calibration = false;
};

// Calibration record (sensor-model.md §3.1, migration 0006). The validity
// interval is half-open [valid_from_ns, valid_to_ns); valid_to_ns nullopt
// means open-ended (valid for every timestamp >= valid_from_ns).
struct CalibrationRow {
  Uuid calibration_id{};
  Uuid sensor_id{};
  std::int64_t version = 1;
  std::int64_t calibration_time_ns = 0;
  std::string source;
  std::string intrinsics_json;
  std::string distortion_json;
  std::string extrinsics_json;
  std::string uncertainty_json;
  std::optional<std::int64_t> valid_from_ns;  // inclusive, always present
  std::optional<std::int64_t> valid_to_ns;    // exclusive, nullopt = open-ended
};

// Mutable (non-identity) sensor attributes (sensor-model.md §1). sensor_id is
// immutable; edits are new sensors or new versions, never in-place mutation.
// Empty strings clear the underlying column to NULL.
struct SensorMetadataUpdate {
  std::string manufacturer;
  std::string model;
  std::string serial_number;
  std::string status;
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

  // Raw connection handle. The engine's scheduler state store
  // (engine/scheduler/scheduler_state_store.cpp) owns its own tables
  // (migration 0003) through this handle; MetadataDb remains the single
  // writer of the SQLite database (ADR-020) and outlives the store.
  sqlite3* db() const noexcept { return db_; }

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
  // Reverse lookup: artifact UUID -> index row. The P2.3 read-boundary
  // extension (a) bridge from observation.artifact_ref (UUID) to the pipeline
  // content-hash input (read-boundary-review.md, RFC-0007 §4).
  std::optional<ArtifactIndexRow> FindArtifactById(
      const Uuid& artifact_id) const;
  std::vector<ArtifactIndexRow> FindArtifactsByType(
      const std::string& type) const;
  void RecordDependency(const std::string& input_hash,
                        const std::string& output_hash, const std::string& role);

  // P2.1 (RFC-0006): scene/capture write records. No new migrations;
  // these tables are schema-resident since 0001.
  //
  // Resolves an existing session by id, returning nullopt when absent.
  std::optional<Uuid> FindSession(const Uuid& session_id);
  void InsertCaptureSession(const CaptureSessionRow& row);

  // Resolves a sensor (by id) and whether it has a registered calibration.
  // Migration 0001 carries only a scalar calibration_time_ns (no validity
  // interval columns), so calibration resolution is "sensor has >=1 row in
  // calibrations" for now; interval-aware validity arrives with P2.2
  // registration. The caller maps the absence to a warning, never a failure.
  std::optional<SensorRow> FindSensor(const Uuid& sensor_id) const;

  // P2.2 (RFC-0002 §1 + sensor-model.md, migration 0006): sensor and
  // calibration registration. sensor_id is immutable: RegisterSensor rejects
  // duplicates; UpdateSensorMetadata touches non-identity attributes only;
  // AddCalibration is append-only (version = previous + 1) and maintains the
  // sensors.calibration_id "latest" pointer, which MUST NOT participate in
  // historical resolution.
  void RegisterSensor(const SensorRow& row);
  void UpdateSensorMetadata(const Uuid& sensor_id,
                            const SensorMetadataUpdate& metadata);
  void AddCalibration(const CalibrationRow& row);

  // Half-open interval resolution: a calibration is valid at timestamp_ns iff
  // valid_from_ns <= timestamp_ns < valid_to_ns, or timestamp_ns >=
  // valid_from_ns when valid_to_ns IS NULL. Returns nullopt when no interval
  // covers the timestamp (gap, before the first, or at/after an interval end).
  std::optional<CalibrationRow> ResolveCalibrationAt(
      const Uuid& sensor_id, std::int64_t timestamp_ns) const;

  // P2.2-B (RFC-0002 §8, ADR-035): typed read surface for the Scene Query
  // API. Read-only; returns row structs that the query layer maps to strict
  // domain types. Sessions and scenes resolve through the project spine: a
  // session belongs to a project, a project owns one open scene.
  std::optional<CaptureSessionRow> FindCaptureSession(
      const Uuid& session_id) const;
  std::optional<SceneRow> FindSceneByProject(const Uuid& project_id) const;

  std::vector<FrameRow> FindFramesByScene(const Uuid& scene_id) const;
  std::vector<FrameRow> FindFramesBySession(const Uuid& session_id) const;
  std::vector<FrameRow> FindFramesBySensor(const Uuid& sensor_id) const;
  std::vector<FrameRow> FindFrames() const;
  // Half-open [from_ns, to_ns), consistent with calibration validity.
  std::vector<FrameRow> FindFramesInTimeRange(std::int64_t from_ns,
                                              std::int64_t to_ns) const;

  std::vector<ObservationRow> FindObservationsByScene(
      const Uuid& scene_id) const;
  std::vector<ObservationRow> FindObservationsByFrame(
      const Uuid& frame_id) const;
  std::vector<ObservationRow> FindObservationsBySession(
      const Uuid& session_id) const;
  std::vector<ObservationRow> FindObservationsBySensor(
      const Uuid& sensor_id) const;
  std::vector<ObservationRow> FindObservations() const;
  std::vector<ObservationRow> FindObservationsInTimeRange(
      std::int64_t from_ns, std::int64_t to_ns) const;

  // Scenes are the parent container for frames and observations. If no scene
  // exists for the project, creates one with version 1 (ADR-033). Returns the
  // current scene with its active version.
  SceneRow FindOrCreateScene(const Uuid& project_id, const std::string& name,
                             const std::string& created_by_json,
                             std::int64_t created_at_ns);

  // Appends a new scene version (parent = current active version), advances
  // scenes.current_version_id, and returns the new active version. Used by
  // the importer to record an "imported" stage (image-import.md §5).
  SceneVersionRow CreateSceneVersion(const Uuid& scene_id,
                                     const std::string& stage,
                                     const std::string& created_by_json,
                                     std::int64_t created_at_ns);

  // P2.3 (RFC-0007 §5): read accessors for the scene_versions table — the
  // immutable, append-only version chain (ADR-033). Read-only; gap (b) of
  // read-boundary-review.md.
  std::optional<SceneVersionRow> FindSceneVersion(
      const Uuid& version_id) const;
  std::vector<SceneVersionRow> FindSceneVersionsByScene(
      const Uuid& scene_id) const;

  void InsertFrame(const FrameRow& row);
  void InsertObservation(const ObservationRow& row);
  void InsertObservationPayload(const ObservationPayloadRow& row);

  // Persistent rejection records (RFC-0006 §14, migration 0005). Inserted by
  // the importer for every per-file failure; a rejected input never writes an
  // artifact or canonical record.
  void InsertImportRejection(const ImportRejectionRow& row);
  std::vector<ImportRejectionRow> FindImportRejectionsBySession(
      const Uuid& session_id) const;

  // Idempotent re-import check (image-import.md §13 case 1). Frames and
  // observations are immutable append-only records (PPS-0001 §5.3); a second
  // insert violates the PK, so the importer queries existence before writing.
  bool FrameExists(const Uuid& frame_id);
  bool ObservationExists(const Uuid& observation_id);

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
