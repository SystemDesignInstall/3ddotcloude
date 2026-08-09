#pragma once

// Image importer (RFC-0006 §6.8, image-import.md). The importer is the only
// place that touches a source file: it reads bytes, detects the format by
// magic bytes, reads header-only geometry (width/height/pixel_format),
// computes SHA-256, resolves session/sensor/timestamp, calls Put or
// PutInstance per the dedup policy (image-import.md §13), and records one
// Frame + one ImageObservation per accepted image inside one CaptureSession.
// RAW families (CR2/NEF/ARW/DNG) are imported as opaque byte payloads —
// never decoded at import (image-import.md §14). No external image library
// crosses the boundary (PPS-0001 §5.1).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/errors/project_error.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"
#include "importers/images/image_format.h"

namespace spatial::importers {

using core::ArtifactManifest;
using core::ArtifactStore;
using core::ErrorCode;
using core::MetadataDb;
using core::Uuid;

// One source image plus its capture context (image-import.md §3).
struct ImageSourceFile {
  std::filesystem::path path;     // local file the importer reads
  std::string source_uri;         // original location (portable URI, ADR-008)
  Uuid sensor_id{};               // which camera recorded the exposure
  std::int64_t timestamp_ns = 0;  // capture time in the platform time domain
};

struct ImageImporterConfig {
  // Format allowlist (image-import.md §3). A file whose magic is not listed
  // is rejected with IMPORT_UNSUPPORTED_FORMAT, never transcoded.
  std::vector<ImageFormat> allowlist = {ImageFormat::kJpeg, ImageFormat::kPng,
                                        ImageFormat::kTiff, ImageFormat::kExr,
                                        ImageFormat::kRaw};
  // Batch name for an implicitly created session (image-import.md §7).
  std::string batch_name = "import";

  // Deterministic SHA-256 of the canonical config JSON; the manifest's
  // configuration_hash (image-import.md §8, §15).
  std::string ConfigurationHash() const;
};

struct ImageImportEntry {
  Uuid frame_id{};
  Uuid observation_id{};
  // New artifact instance written by this import; nil for an idempotent
  // re-import (dedup case 1) where nothing was written.
  Uuid artifact_uuid{};
  std::string content_hash;
  std::string mime_type;
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::string pixel_format;
  bool reimported = false;   // dedup case 1: tuple already present, no writes
  bool new_instance = false;  // dedup case 2: new instance, shared CAS payload
  bool uncalibrated = false;  // calibration_unresolved warning, not a failure
};

struct ImageImportFailure {
  std::string source_uri;
  ErrorCode code = ErrorCode::kImportValidationError;
  std::string diagnostic;
};

struct ImageImportResult {
  Uuid session_id{};  // the batch session (explicit or newly created)
  std::vector<ImageImportEntry> imported;
  std::vector<ImageImportFailure> failures;
};

class ImageImporter {
 public:
  // `artifacts` and `db` must outlive the importer; `project_id` owns all
  // scene/session/frame/observation records.
  ImageImporter(ArtifactStore& artifacts, MetadataDb& db, Uuid project_id,
                const ImageImporterConfig& config);

  // Imports a batch. Files are processed in sorted source-URI order
  // (deterministic, image-import.md §15). Per-file failures are collected,
  // never thrown; the batch continues (image-import.md §12). A failed file
  // never partially writes. An explicit session must exist, else ImportError
  // (image-import.md §7). Throws ImportError on an empty batch.
  ImageImportResult Import(
      const std::vector<ImageSourceFile>& files,
      const std::optional<Uuid>& explicit_session = std::nullopt);

 private:
  std::optional<ImageImportEntry> ProcessFile(const ImageSourceFile& file,
                                              std::int64_t sequence_index,
                                              const Uuid& scene_id,
                                              const Uuid& session_id,
                                              ImageImportFailure* out_failure);
  // Persists the rejection of an input (RFC-0006 §14, migration 0005) with
  // its provenance (path, detected mime, importer id/version, stable
  // IMPORT_* code, timestamp). Called once per per-file failure; a rejected
  // file never writes an artifact or canonical record.
  void RecordRejection(const Uuid& session_id, const std::string& uri,
                       const std::string& mime, ErrorCode code,
                       const std::string& diagnostic,
                       std::int64_t sequence_index) const;
  std::string ProducerJson() const;
  ArtifactManifest MakeManifest(const ImageHeaderInfo& header,
                                std::size_t file_size) const;

  ArtifactStore& artifacts_;
  MetadataDb& db_;
  Uuid project_id_{};
  ImageImporterConfig config_;
};

}  // namespace spatial::importers
