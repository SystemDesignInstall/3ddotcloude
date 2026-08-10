// Scene Query API — typed mapping from storage rows to strict domain types
// (ADR-035, P2.2-B). See scene_query.h for scope. SQL lives in
// core/storage/metadata_db.cpp; this layer never touches SQL or raw rows.

#include "core/scene/query/scene_query.h"

#include <cstdint>
#include <utility>

#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"

namespace spatial::core {

namespace {

CaptureSession ToDomain(const CaptureSessionRow& row) {
  CaptureSession out;
  out.session_id = row.session_id;
  out.project_id = row.project_id;
  out.name = row.name;
  out.source_uri = row.source_uri;
  out.started_at = TimestampNs(row.started_at_ns);
  out.ended_at = TimestampNs(row.ended_at_ns);
  out.status = row.status;
  out.provenance_json = row.provenance_json;
  return out;
}

Scene ToDomain(const SceneRow& row) {
  Scene out;
  out.scene_id = row.scene_id;
  out.schema_version = row.schema_version;
  out.project_id = row.project_id;
  out.name = row.name;
  out.version_id = row.version_id;
  out.parent_version_id = row.parent_version_id;
  out.stage = row.stage;
  out.created_by_json = row.created_by_json;
  out.created_at_ns = row.created_at_ns;
  out.origin_frame = row.origin_frame;
  out.crs = row.crs;
  out.status = row.status;
  out.properties_json = row.properties_json;
  return out;
}

Frame ToDomain(const FrameRow& row) {
  Frame out;
  out.frame_id = row.frame_id;
  out.scene_id = row.scene_id;
  out.session_id = row.session_id;
  out.timestamp_ns = TimestampNs(row.timestamp_ns);
  out.sequence_index = row.sequence_index;
  out.sensor_id = row.sensor_id;
  out.pose_ref = row.pose_ref;
  out.properties_json = row.properties_json;
  return out;
}

ImageObservation ToDomain(const ObservationRow& row) {
  ImageObservation out;
  out.observation_id = row.observation_id;
  out.scene_id = row.scene_id;
  out.sensor_id = row.sensor_id;
  out.frame_id = row.frame_id;
  out.session_id = row.session_id;
  out.timestamp_ns = TimestampNs(row.timestamp_ns);
  if (!row.artifact_ref.empty()) out.artifact_ref = ParseUuid(row.artifact_ref);
  out.source_json = row.source_json;
  out.properties_json = row.properties_json;
  out.width = row.width;
  out.height = row.height;
  out.pixel_format = row.pixel_format;
  return out;
}

Sensor ToDomain(const SensorRow& row) {
  Sensor out;
  out.sensor_id = row.sensor_id;
  out.project_id = row.project_id;
  out.type = row.type;
  out.manufacturer = row.manufacturer;
  out.model = row.model;
  out.serial_number = row.serial_number;
  out.time_domain = row.time_domain;
  out.calibration_id = row.calibration_id;
  out.rig_id = row.rig_id;
  out.source_json = row.source_json;
  out.status = row.status;
  out.has_calibration = row.has_calibration;
  return out;
}

Calibration ToDomain(const CalibrationRow& row) {
  Calibration out;
  out.calibration_id = row.calibration_id;
  out.sensor_id = row.sensor_id;
  out.version = row.version;
  out.calibration_time_ns = TimestampNs(row.calibration_time_ns);
  out.source = row.source;
  out.intrinsics_json = row.intrinsics_json;
  out.distortion_json = row.distortion_json;
  out.extrinsics_json = row.extrinsics_json;
  out.uncertainty_json = row.uncertainty_json;
  if (row.valid_from_ns) out.valid_from = TimestampNs(*row.valid_from_ns);
  if (row.valid_to_ns) out.valid_to = TimestampNs(*row.valid_to_ns);
  return out;
}

SceneVersion ToDomain(const SceneVersionRow& row) {
  SceneVersion out;
  out.version_id = row.version_id;
  out.scene_id = row.scene_id;
  out.parent_version_id = row.parent_version_id;
  out.stage = row.stage;
  out.created_by_json = row.created_by_json;
  out.created_at_ns = row.created_at_ns;
  out.status = row.status;
  return out;
}

std::vector<ImageObservation> ImageObservationsFrom(
    std::vector<ObservationRow> rows) {
  std::vector<ImageObservation> out;
  for (auto& row : rows) {
    if (row.type == "image") out.push_back(ToDomain(row));
  }
  return out;
}

}  // namespace

std::optional<CaptureSession> SceneQuery::FindCaptureSession(
    const Uuid& session_id) const {
  const auto row = db_.FindCaptureSession(session_id);
  if (!row) return std::nullopt;
  return ToDomain(*row);
}

std::optional<Scene> SceneQuery::FindSceneByProject(
    const Uuid& project_id) const {
  const auto row = db_.FindSceneByProject(project_id);
  if (!row) return std::nullopt;
  return ToDomain(*row);
}

std::optional<Scene> SceneQuery::SessionScene(const Uuid& session_id) const {
  const auto session = db_.FindCaptureSession(session_id);
  if (!session) return std::nullopt;
  return FindSceneByProject(session->project_id);
}

std::optional<Sensor> SceneQuery::ResolveSensor(const Uuid& sensor_id) const {
  const auto row = db_.FindSensor(sensor_id);
  if (!row) return std::nullopt;
  return ToDomain(*row);
}

std::optional<Calibration> SceneQuery::ResolveCalibrationAt(
    const Uuid& sensor_id, TimestampNs timestamp) const {
  const auto row = db_.ResolveCalibrationAt(sensor_id, timestamp.value());
  if (!row) return std::nullopt;
  return ToDomain(*row);
}

std::vector<Frame> SceneQuery::Frames() const {
  std::vector<Frame> out;
  for (const auto& row : db_.FindFrames()) out.push_back(ToDomain(row));
  return out;
}

std::vector<Frame> SceneQuery::FramesBySession(const Uuid& session_id) const {
  std::vector<Frame> out;
  for (const auto& row : db_.FindFramesBySession(session_id)) {
    out.push_back(ToDomain(row));
  }
  return out;
}

std::vector<Frame> SceneQuery::FramesBySensor(const Uuid& sensor_id) const {
  std::vector<Frame> out;
  for (const auto& row : db_.FindFramesBySensor(sensor_id)) {
    out.push_back(ToDomain(row));
  }
  return out;
}

std::vector<Frame> SceneQuery::FramesInTimeRange(TimestampNs from,
                                                 TimestampNs to) const {
  std::vector<Frame> out;
  for (const auto& row : db_.FindFramesInTimeRange(from.value(), to.value())) {
    out.push_back(ToDomain(row));
  }
  return out;
}

std::vector<ImageObservation> SceneQuery::Observations() const {
  return ImageObservationsFrom(db_.FindObservations());
}

std::vector<ImageObservation> SceneQuery::ObservationsByFrame(
    const Uuid& frame_id) const {
  return ImageObservationsFrom(db_.FindObservationsByFrame(frame_id));
}

std::vector<ImageObservation> SceneQuery::ObservationsBySession(
    const Uuid& session_id) const {
  return ImageObservationsFrom(db_.FindObservationsBySession(session_id));
}

std::vector<ImageObservation> SceneQuery::ObservationsBySensor(
    const Uuid& sensor_id) const {
  return ImageObservationsFrom(db_.FindObservationsBySensor(sensor_id));
}

std::vector<ImageObservation> SceneQuery::ObservationsInTimeRange(
    TimestampNs from, TimestampNs to) const {
  return ImageObservationsFrom(
      db_.FindObservationsInTimeRange(from.value(), to.value()));
}

std::optional<std::string> SceneQuery::ArtifactHash(
    const Uuid& artifact_uuid) const {
  const auto row = db_.FindArtifactById(artifact_uuid);
  if (!row) return std::nullopt;
  return row->content_hash;
}

std::optional<SceneVersion> SceneQuery::SceneVersion(
    const Uuid& version_id) const {
  const auto row = db_.FindSceneVersion(version_id);
  if (!row) return std::nullopt;
  return ToDomain(*row);
}

std::vector<SceneVersion> SceneQuery::SceneVersions(
    const Uuid& scene_id) const {
  std::vector<::spatial::core::SceneVersion> out;
  for (const auto& row : db_.FindSceneVersionsByScene(scene_id)) {
    out.push_back(ToDomain(row));
  }
  return out;
}

}  // namespace spatial::core
