#include "importers/images/image_importer.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/scene/identity.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "image_import_build_info.h"

namespace spatial::importers {

std::string ImageImporterConfig::ConfigurationHash() const {
  nlohmann::json j;
  j["allowlist"] = nlohmann::json::array();
  for (const auto& f : allowlist) j["allowlist"].push_back(ToString(f));
  std::sort(j["allowlist"].begin(), j["allowlist"].end());
  j["batch_name"] = batch_name;
  j["raw_policy"] = "opaque";
  // nlohmann objects serialize with sorted keys; the array is sorted above, so
  // the digest is canonical (image-import.md §15).
  return Sha256Hex(j.dump());
}

ImageImporter::ImageImporter(ArtifactStore& artifacts, MetadataDb& db,
                             Uuid project_id,
                             const ImageImporterConfig& config)
    : artifacts_(artifacts),
      db_(db),
      project_id_(std::move(project_id)),
      config_(config) {}

std::string ImageImporter::ProducerJson() const {
  return nlohmann::json{{"id", "image-import"},
                        {"version", kImageImportVersion},
                        {"git_commit", kImageImportGitCommit}}
      .dump();
}

ArtifactManifest ImageImporter::MakeManifest(const ImageHeaderInfo& header,
                                             std::size_t file_size) const {
  ArtifactManifest m;
  m.type = "image";
  m.schema_version = 1;
  m.producer.id = "image-import";
  m.producer.version = kImageImportVersion;
  m.producer.git_commit = kImageImportGitCommit;
  m.configuration_hash = config_.ConfigurationHash();
  m.coordinate_frame = "image";
  m.unit = "meter";
  m.file_size = static_cast<std::int64_t>(file_size);
  m.mime_type = header.mime_type;
  m.validation_status = "valid";
  m.width = header.width;
  m.height = header.height;
  m.pixel_format = header.pixel_format;
  return m;
}

ImageImportResult ImageImporter::Import(
    const std::vector<ImageSourceFile>& files,
    const std::optional<Uuid>& explicit_session) {
  if (files.empty()) {
    throw ImportError(ErrorCode::kImportValidationError,
                      "import batch is empty", {}, false,
                      "Provide at least one image file to import.");
  }

  // Deterministic processing order (image-import.md §15).
  auto ordered = files;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const ImageSourceFile& a, const ImageSourceFile& b) {
                     return a.source_uri < b.source_uri;
                   });

  // Session: an explicit session must exist; otherwise one session is created
  // per batch (image-import.md §7). A batch never spans sessions.
  Uuid session_id{};
  if (explicit_session.has_value()) {
    const auto found = db_.FindSession(*explicit_session);
    if (!found.has_value()) {
      throw ImportError(ErrorCode::kImportValidationError,
                        "explicit capture session not found: " +
                            FormatUuid(*explicit_session),
                        {{"session_id", FormatUuid(*explicit_session)}}, false,
                        "Create the session first, or import without an "
                        "explicit session to create one per batch.");
    }
    session_id = *found;
  } else {
    // Batch session metadata derives from the input timestamps, keeping the
    // records deterministic across re-imports.
    std::int64_t started = std::numeric_limits<std::int64_t>::max();
    std::int64_t ended = std::numeric_limits<std::int64_t>::min();
    for (const auto& f : ordered) {
      started = std::min(started, f.timestamp_ns);
      ended = std::max(ended, f.timestamp_ns);
    }
    CaptureSessionRow row;
    row.session_id = GenerateUuid();
    row.project_id = project_id_;
    row.name = config_.batch_name;
    row.started_at_ns = started;
    row.ended_at_ns = ended;
    row.source_uri = ordered.front().source_uri;
    row.status = "open";
    row.provenance_json = ProducerJson();
    db_.InsertCaptureSession(row);
    session_id = row.session_id;
  }

  // Scene container plus one ADR-033 "imported" version for this import run.
  const std::string created_by = ProducerJson();
  const std::int64_t now_ns = fs::TimestampNsNow();
  const auto scene = db_.FindOrCreateScene(project_id_, config_.batch_name,
                                           created_by, now_ns);
  db_.CreateSceneVersion(scene.scene_id, "imported", created_by, now_ns);

  ImageImportResult result;
  result.session_id = session_id;

  std::int64_t sequence = 0;
  for (const auto& file : ordered) {
    ImageImportFailure failure;
    if (const auto entry =
            ProcessFile(file, sequence++, scene.scene_id, session_id,
                        &failure)) {
      result.imported.push_back(*entry);
    } else {
      result.failures.push_back(std::move(failure));
    }
  }
  return result;
}

std::optional<ImageImportEntry> ImageImporter::ProcessFile(
    const ImageSourceFile& file, std::int64_t sequence_index,
    const Uuid& scene_id, const Uuid& session_id,
    ImageImportFailure* failure) {
  const std::string uri =
      file.source_uri.empty() ? file.path.string() : file.source_uri;

  // Byte-exact read; the importer is the only place that touches a source file.
  std::vector<std::uint8_t> bytes;
  try {
    bytes = fs::ReadFile(file.path);
  } catch (const ProjectError& e) {
    *failure = {uri, ErrorCode::kImportUnreadable, e.message()};
    return std::nullopt;
  }

  // Format by magic bytes, then the allowlist (image-import.md §3, §14).
  const ImageFormat format = DetectImageFormat(bytes.data(), bytes.size());
  const bool allowed = std::find(config_.allowlist.begin(),
                                 config_.allowlist.end(), format) !=
                       config_.allowlist.end();
  if (format == ImageFormat::kUnknown || !allowed) {
    *failure = {uri, ErrorCode::kImportUnsupportedFormat,
                "format not in the import allowlist"};
    return std::nullopt;
  }

  // Header-only geometry; RAW stays an opaque payload.
  const auto header = ParseImageHeader(bytes.data(), bytes.size());
  if (!header || header->width < 1 || header->height < 1 ||
      header->pixel_format.empty()) {
    *failure = {uri, ErrorCode::kImportCorrupt,
                "header is truncated or has no image geometry"};
    return std::nullopt;
  }

  // Sensor and time resolution (image-import.md §5): both are required; an
  // unresolvable sensor is a validation error, an uncalibrated sensor is only
  // a warning.
  const auto sensor = db_.FindSensor(file.sensor_id);
  if (!sensor) {
    *failure = {uri, ErrorCode::kImportSensorUnresolved,
                "no registered sensor: " + FormatUuid(file.sensor_id)};
    return std::nullopt;
  }
  if (file.timestamp_ns == 0) {
    *failure = {uri, ErrorCode::kImportTimestampUnresolvable,
                "capture time cannot resolve to the platform time domain"};
    return std::nullopt;
  }

  // Deterministic v5 identity (image-import.md §6).
  const std::string hash = Sha256Hex(bytes);
  const Uuid frame_id = DeriveFrameId(file.sensor_id, file.timestamp_ns, hash);
  const Uuid observation_id =
      DeriveObservationId(file.sensor_id, file.timestamp_ns, hash);

  ImageImportEntry entry;
  entry.frame_id = frame_id;
  entry.observation_id = observation_id;
  entry.content_hash = hash;
  entry.mime_type = header->mime_type;
  entry.width = header->width;
  entry.height = header->height;
  entry.pixel_format = header->pixel_format;
  entry.uncalibrated = !sensor->has_calibration;

  // Dedup case 1 (image-import.md §13): the identical tuple is already
  // imported; re-import is idempotent and writes nothing.
  if (db_.ObservationExists(observation_id)) {
    entry.reimported = true;
    return entry;
  }

  // Payload write. Content already present means a content-identical import
  // in a different context (case 2): reuse the single CAS payload through a
  // new immutable instance. New content goes through the plain Put path.
  auto manifest = MakeManifest(*header, bytes.size());
  ArtifactWriteResult written;
  if (db_.FindArtifactByHash(hash)) {
    written = artifacts_.PutInstance(hash, manifest);
    entry.new_instance = true;
  } else {
    written = artifacts_.Put(bytes, manifest);
  }
  entry.artifact_uuid = written.artifact_uuid;
  entry.content_hash = written.content_hash;

  // Canonical records: Frame + ImageObservation + observation payload, all
  // inside the batch session (image-import.md §4). Immutable once written
  // (PPS-0001 §5.3); a failure above means none of them exist.
  FrameRow frame;
  frame.frame_id = frame_id;
  frame.scene_id = scene_id;
  frame.session_id = session_id;
  frame.timestamp_ns = file.timestamp_ns;
  frame.sequence_index = sequence_index;
  frame.sensor_id = file.sensor_id;
  frame.pose_ref = {};
  db_.InsertFrame(frame);

  ObservationRow observation;
  observation.observation_id = observation_id;
  observation.scene_id = scene_id;
  observation.sensor_id = file.sensor_id;
  observation.frame_id = frame_id;
  observation.session_id = session_id;
  observation.timestamp_ns = file.timestamp_ns;
  observation.type = "image";
  observation.artifact_ref = FormatUuid(written.artifact_uuid);
  observation.source_json = ProducerJson();
  observation.properties_json =
      nlohmann::json{{"configuration_hash", config_.ConfigurationHash()}}
          .dump();
  db_.InsertObservation(observation);

  ObservationPayloadRow payload;
  payload.observation_id = observation_id;
  payload.width = header->width;
  payload.height = header->height;
  payload.pixel_format = header->pixel_format;
  db_.InsertObservationPayload(payload);

  return entry;
}

}  // namespace spatial::importers
