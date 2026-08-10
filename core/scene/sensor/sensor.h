#pragma once

// Sensor and Calibration domain types (sensor-model.md §1, §3.1, RFC-0002
// §6.4; normative for core/scene/sensor/**). sensor_id and rig_id are
// immutable; calibrations are append-only and versioned (history never
// rewritten). calibration_id is the maintained "latest" pointer ONLY — it
// never participates in historical resolution (P2.2 §3 rule). Timestamps use
// the platform's strict types (ADR-018).

#include <cstdint>
#include <optional>
#include <string>

#include "core/coordinates/timestamp.h"
#include "core/utils/uuid.h"

namespace spatial::core {

struct Sensor {
  Uuid sensor_id{};             // immutable identity
  Uuid project_id{};
  std::string type;             // camera | lidar | imu | gnss | rgbd | ...
  std::string manufacturer;
  std::string model;
  std::string serial_number;
  std::string time_domain;      // device | gps | platform (sensor-model.md §4)
  Uuid calibration_id{};        // latest-pointer; nil UUID == none registered
  Uuid rig_id{};                // nil UUID == not rigged
  std::string source_json;      // ProducerInfo: app + version + git commit
  std::string status = "active";
  bool has_calibration = false;  // >= 1 registered calibration interval
};

struct Calibration {
  Uuid calibration_id{};
  Uuid sensor_id{};
  std::int64_t version = 1;     // monotonically increasing, append-only
  TimestampNs calibration_time_ns{0};  // production time, not validity
  std::string source;           // ProducerInfo
  std::string intrinsics_json;
  std::string distortion_json;
  std::string extrinsics_json;
  std::string uncertainty_json;
  // Half-open validity interval [valid_from, valid_to); nullopt valid_to means
  // open-ended (valid for every timestamp >= valid_from).
  std::optional<TimestampNs> valid_from;
  std::optional<TimestampNs> valid_to;
};

}  // namespace spatial::core
