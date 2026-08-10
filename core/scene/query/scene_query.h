#pragma once

// Scene Query API — the P2.2 typed read subset of ADR-035 scene.query().
//
// Read-only by construction: wraps a const MetadataDb& and exposes no write
// methods. Returns strict domain types (core::scene::Frame, ImageObservation,
// CaptureSession, Scene, Sensor, Calibration) — never raw rows, never SQL,
// never storage internals. SQL stays in core/storage/.
//
// P2.2-B in scope: Frame/Observation queries by session/sensor/time-range,
// Session->Scene traversal, Scene/CaptureSession/Sensor reads, and calibration
// resolution. Out of scope (post-M0, ADR-035): .lidar()/.points()/
// .visibleFrom() geometry-graph joins, .quality()/.uncertainty() channels,
// spatial-index-accelerated queries, and any write/mutation surface.

#include <optional>
#include <vector>

#include "core/coordinates/timestamp.h"
#include "core/scene/capture_session.h"
#include "core/scene/frame.h"
#include "core/scene/observation_graph/image_observation.h"
#include "core/scene/scene.h"
#include "core/scene/sensor/sensor.h"
#include "core/utils/uuid.h"

namespace spatial::core {

class MetadataDb;

class SceneQuery {
 public:
  explicit SceneQuery(const MetadataDb& db) : db_(db) {}

  // --- Sessions and scenes -------------------------------------------------
  std::optional<CaptureSession> FindCaptureSession(
      const Uuid& session_id) const;
  std::optional<Scene> FindSceneByProject(const Uuid& project_id) const;
  // Session->Scene traversal (plan §5): session -> owning project -> the
  // project's open scene. Returns nullopt when the session or its scene is
  // absent.
  std::optional<Scene> SessionScene(const Uuid& session_id) const;

  // --- Sensors and calibration ---------------------------------------------
  std::optional<Sensor> ResolveSensor(const Uuid& sensor_id) const;
  // Half-open [valid_from, valid_to); nullopt when no interval covers the
  // timestamp (gap, before first, or at/after an interval end). The scalar
  // sensors.calibration_id pointer never participates in resolution.
  std::optional<Calibration> ResolveCalibrationAt(
      const Uuid& sensor_id, TimestampNs timestamp) const;

  // --- Frames --------------------------------------------------------------
  std::vector<Frame> Frames() const;
  std::vector<Frame> FramesBySession(const Uuid& session_id) const;
  std::vector<Frame> FramesBySensor(const Uuid& sensor_id) const;
  // Half-open [from, to), consistent with calibration validity.
  std::vector<Frame> FramesInTimeRange(TimestampNs from, TimestampNs to) const;

  // --- Observations ---------------------------------------------------------
  // Image subtype only (the sole representable observation today); rows of
  // other types are excluded until the geometry-graph joins land (ADR-035).
  std::vector<ImageObservation> Observations() const;
  std::vector<ImageObservation> ObservationsByFrame(
      const Uuid& frame_id) const;
  std::vector<ImageObservation> ObservationsBySession(
      const Uuid& session_id) const;
  std::vector<ImageObservation> ObservationsBySensor(
      const Uuid& sensor_id) const;
  std::vector<ImageObservation> ObservationsInTimeRange(TimestampNs from,
                                                        TimestampNs to) const;

 private:
  const MetadataDb& db_;
};

}  // namespace spatial::core
