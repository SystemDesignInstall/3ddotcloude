#pragma once

// Calibration -> CalibrationArtifact materialization (RFC-0009 §6). The
// session layer resolves the scene's Calibration record via
// SceneQuery::ResolveCalibrationAt, then materializes it into the immutable
// CAS CalibrationArtifact whose content hash travels in TaskRequest.input_refs.
// This mirrors the two-tier pattern of the feature artifact: the scene-agnostic
// payload writer lives in core/artifacts/calibration_artifact.h; this adapter
// sits on the accepted SceneQuery read boundary and supplies the declared
// fields from the resolved record. The scene record remains the runtime
// authority; the artifact is its immutable, content-addressed backing payload
// (RFC-0009 §3-§4).

#include <string>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/artifacts/calibration_artifact.h"
#include "core/scene/sensor/sensor.h"

namespace spatial::core {

struct MaterializeCalibrationOptions {
  ProducerInfo producer;                  // solver/calibrator or session layer
  std::vector<std::string> input_artifact_hashes;  // may be empty (factory)
  std::string configuration_hash;         // calibration computation config
  // Coordinate frame of the extrinsics, e.g. "rig_0"/"world"; empty for
  // intrinsic-only calibration.
  std::string coordinate_frame;
};

// Serializes the resolved scene `Calibration` record into the canonical
// CalibrationArtifact payload (calibration.schema.json) and writes payload +
// manifest into `store`. Intrinsics/distortion/extrinsics/uncertainty come
// from the record's declarative JSON columns verbatim. Returns the CAS result
// (content hash + artifact uuid); identical records deduplicate (ADR-010).
CalibrationArtifactResult MaterializeCalibrationArtifact(
    ArtifactStore& store, const Calibration& calibration,
    const MaterializeCalibrationOptions& options);

}  // namespace spatial::core
