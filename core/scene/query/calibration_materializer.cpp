#include "core/scene/query/calibration_materializer.h"

#include <cstdint>
#include <optional>
#include <string>

#include "core/artifacts/calibration_artifact.h"
#include "core/scene/sensor/sensor.h"
#include "core/utils/uuid.h"

namespace spatial::core {

CalibrationArtifactResult MaterializeCalibrationArtifact(
    ArtifactStore& store, const Calibration& calibration,
    const MaterializeCalibrationOptions& options) {
  WriteCalibrationArtifactInput input;
  input.calibration_id = FormatUuid(calibration.calibration_id);
  input.sensor_id = FormatUuid(calibration.sensor_id);
  input.version = calibration.version;
  input.calibration_time_ns = calibration.calibration_time_ns.value();
  input.source = calibration.source;
  input.intrinsics_json = calibration.intrinsics_json;
  input.distortion_json = calibration.distortion_json;
  input.extrinsics_json = calibration.extrinsics_json;
  input.uncertainty_json = calibration.uncertainty_json;
  if (calibration.valid_from) {
    input.valid_from_ns = calibration.valid_from->value();
  }
  if (calibration.valid_to) {
    input.valid_to_ns = calibration.valid_to->value();
  }
  input.input_artifact_hashes = options.input_artifact_hashes;
  input.configuration_hash = options.configuration_hash;
  input.coordinate_frame = options.coordinate_frame;
  input.producer = options.producer;
  return WriteCalibrationArtifact(store, input);
}

}  // namespace spatial::core
