#pragma once

// CalibrationArtifact (RFC-0009, ratified). A canonical artifact of type
// "calibration" whose payload is a JSON document conforming to
// schemas/json/calibration.schema.json (the Canonical Camera Calibration
// Model), with a standard ArtifactManifest (producer, input_artifact_hashes,
// configuration_hash, coordinate_frame, unit, schema_version: 1). It is a
// general spatial artifact — any backend consuming camera intrinsics consumes
// CalibrationArtifacts; any solver/calibrator or the session layer produces
// them. It is never named after a vendor.
//
// The writer here is scene-agnostic (ADR-038): it takes the already-declared
// calibration fields, produces the canonical payload bytes as a pure function
// of them (so identical calibration content dedupes in the CAS, ADR-010), and
// writes payload + manifest into the store. It never touches the metadata DB
// or scene rows. The scene-record -> artifact materialization lives in
// core/scene/query/calibration_materializer.h (session layer, RFC-0009 §6).
//
// Invariants (RFC-0009 §6): calibration travels as a content hash in
// TaskRequest.input_refs, never inside config_json; a calibration value in the
// configuration surface is a contract violation.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/utils/uuid.h"

namespace spatial::core {

struct WriteCalibrationArtifactInput {
  std::string calibration_id;         // canonical RFC-4122 string
  std::string sensor_id;              // canonical RFC-4122 string
  std::int64_t version = 1;           // monotonic, append-only
  std::int64_t calibration_time_ns = 0;  // production time, not validity
  std::string source;                 // provenance (factory/self-calibration/import)
  // Canonical camera model vocabulary (calibration.schema.json
  // intrinsic_model, RFC-0009 §5). Empty -> omitted from the payload.
  std::string intrinsic_model;
  std::string intrinsics_json;        // e.g. {"fx":..,"fy":..,"cx":..,"cy":..}
  std::string distortion_json;        // may be empty
  std::string extrinsics_json;        // may be empty
  std::string uncertainty_json;       // may be empty
  std::optional<std::int64_t> valid_from_ns;  // inclusive
  std::optional<std::int64_t> valid_to_ns;    // exclusive; nullopt = open-ended
  std::vector<std::string> input_artifact_hashes;  // provenance inputs (may be empty)
  std::string configuration_hash;     // calibration computation config digest;
                                      // empty when no computation produced it
  std::string coordinate_frame;       // rig/world when extrinsics present;
                                      // empty for intrinsic-only
  ProducerInfo producer;              // solver/calibrator or session layer
};

struct CalibrationArtifactResult {
  Uuid artifact_uuid{};
  std::string content_hash;
  bool deduplicated = false;
};

// Writes ONLY the CalibrationArtifact payload + manifest into `store` (type
// "calibration", mime application/json, unit "meter"). The payload conforms to
// calibration.schema.json; malformed declared fields (non-JSON intrinsics/
// distortion/extrinsics/uncertainty) are rejected with ProjectError
// kValidationDomain. Scene-agnostic (ADR-038): no sensor/scene knowledge is
// required. The produced bytes are a pure function of the inputs, so identical
// calibration content dedupes in the CAS and replay from the ADR-020 task
// cache (AC-8).
CalibrationArtifactResult WriteCalibrationArtifact(
    ArtifactStore& store, const WriteCalibrationArtifactInput& input);

}  // namespace spatial::core
