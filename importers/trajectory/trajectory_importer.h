#pragma once

// Camera trajectory import at the project/session boundary. The source bytes
// and canonical pose payload are immutable CAS artifacts. Importing a file
// does not establish a metric basis.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"

namespace spatial::importers {

enum class TrajectoryLengthUnit { kMeters };
enum class TrajectoryClockDomain { kPlatform };
enum class TrajectoryPoseFrame { kCamera };

struct TrajectoryImportOptions {
  core::Uuid session_id{};
  core::Uuid sensor_id{};
  std::string coordinate_frame;
  // Optional operator-supplied identity of the system that produced the file.
  std::string source_producer;
  std::string source_version;
  // Each declaration is required. A unit declaration and equal timestamps
  // do not authenticate physical scale or synchronize clocks.
  std::optional<TrajectoryLengthUnit> source_unit;
  std::optional<TrajectoryClockDomain> clock_domain;
  std::optional<TrajectoryPoseFrame> pose_frame;
};

struct TrajectoryImportResult {
  core::Uuid trajectory_id{};
  core::Uuid artifact_id{};
  core::Uuid source_artifact_id{};
  core::Uuid configuration_artifact_id{};
  core::Uuid scene_id{};
  std::string content_hash;
  std::string source_content_hash;
  std::string configuration_hash;
  std::int64_t node_count = 0;
  double total_distance_m = 0.0;
  bool reused = false;
};

// TUM camera poses: timestamp_seconds tx ty tz qx qy qz qw. The caller
// supplies source bytes, never a persisted absolute path. Each pose must match
// exactly one Frame by (session, sensor, timestamp_ns); the caller must first
// place source timestamps in the platform clock domain. Structural and frame
// matching failures happen before publication.
TrajectoryImportResult ImportTumCameraTrajectory(
    core::ArtifactStore& store, core::MetadataDb& db, const core::Uuid& project_id,
    const std::vector<std::uint8_t>& source,
    const TrajectoryImportOptions& options);

}  // namespace spatial::importers
