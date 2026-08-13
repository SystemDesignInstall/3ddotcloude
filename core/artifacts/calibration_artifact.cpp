#include "core/artifacts/calibration_artifact.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"

namespace spatial::core {
namespace {

using nlohmann::json;

// Builds the canonical CalibrationArtifact payload document. nlohmann objects
// serialize with sorted keys, so the bytes (and hence the CAS hash) are a pure
// function of the inputs (RFC-0009 §4, AC-8).
json BuildPayload(const WriteCalibrationArtifactInput& input) {
  json payload = {
      {"calibration_id", input.calibration_id},
      {"sensor_id", input.sensor_id},
      {"version", input.version},
      {"calibration_time_ns", input.calibration_time_ns},
      {"source", input.source},
  };

  if (!input.intrinsic_model.empty()) {
    payload["intrinsic_model"] = input.intrinsic_model;
  }
  payload["intrinsics"] = json::parse(input.intrinsics_json);
  if (!input.distortion_json.empty()) {
    payload["distortion"] = json::parse(input.distortion_json);
  }
  if (!input.extrinsics_json.empty()) {
    payload["extrinsics"] = json::parse(input.extrinsics_json);
  }
  if (!input.uncertainty_json.empty()) {
    payload["uncertainty"] = json::parse(input.uncertainty_json);
  }
  if (input.valid_from_ns) {
    payload["valid_from_ns"] = *input.valid_from_ns;
  }
  if (input.valid_to_ns) {
    payload["valid_to_ns"] = *input.valid_to_ns;
  }
  return payload;
}

}  // namespace

CalibrationArtifactResult WriteCalibrationArtifact(
    ArtifactStore& store, const WriteCalibrationArtifactInput& input) {
  // The declared JSON payload fields must parse (calibration.schema.json
  // conformance is validated separately by the schema gate / tests). A
  // malformed declaration is a caller bug, not something to silently drop.
  try {
    const json payload = BuildPayload(input);
    const std::string payload_text = payload.dump();
    const std::vector<std::uint8_t> bytes(payload_text.begin(),
                                          payload_text.end());

    ArtifactManifest manifest;
    manifest.type = "calibration";
    manifest.schema_version = 1;
    manifest.producer = input.producer;
    manifest.input_artifact_hashes = input.input_artifact_hashes;
    manifest.configuration_hash = input.configuration_hash;
    manifest.coordinate_frame = input.coordinate_frame;
    manifest.unit = "meter";
    manifest.mime_type = "application/json";
    manifest.creation_timestamp = fs::Iso8601UtcNow();

    const auto written = store.Put(bytes, manifest);

    CalibrationArtifactResult result;
    result.artifact_uuid = written.artifact_uuid;
    result.content_hash = written.content_hash;
    result.deduplicated = written.deduplicated;
    return result;
  } catch (const json::parse_error&) {
    throw ProjectError(ErrorCode::kValidationDomain,
                       "calibration artifact: declared JSON field is "
                       "malformed");
  }
}

}  // namespace spatial::core
