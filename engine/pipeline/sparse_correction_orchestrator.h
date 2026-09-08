#pragma once

// P3-Production-E2E — host-runner COMMIT role for the sparse-correction chain.
//
// Worker subprocesses produce a canonical reconstruction artifact in the CAS
// but never touch the MetadataDb (worker boundary, P3-impl-8c §1). This
// host-runner step is the ONLY writer of the reconstruction revision on this
// path: it materializes a worker-proven CAS payload into the project revision
// history as a SUCCEEDED revision, and returns the parsed canonical document
// the next host-runner stage (BundleAdjustmentOptimizePipeline) reads.
//
// Fail-closed (§8/P12): the payload must parse as a canonical reconstruction
// document carrying a non-empty reconstruction_id and a provisional
// "succeeded" status (colmap_converter.cpp:518 writes status="succeeded"), and
// the scene must resolve — otherwise a typed ValidationError is thrown BEFORE
// any database write. The persisted row.document_json is the canonical
// re-serialization of the SAME parsed reconstruction, so equal inputs produce
// an identical document (ADR-020 determinism).

#include <cstdint>
#include <string>
#include <vector>

#include "core/errors/project_error.h"
#include "core/reconstruction/reconstruction.h"
#include "core/reconstruction/reconstruction_json.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"

namespace spatial::engine {

// Commits a worker-produced reconstruction CAS payload as a succeeded revision.
// Throws core::ValidationError (before any write) when the payload is not a
// canonical, provisional-succeeded reconstruction document.
inline spatial::core::Reconstruction CommitWorkerReconstructionArtifact(
    spatial::core::MetadataDb& db, const spatial::core::Uuid& scene_id,
    const std::vector<std::uint8_t>& payload, std::int64_t created_at_ns) {
  spatial::core::Reconstruction rec;
  try {
    rec = spatial::core::ReconstructionFromJson(
        std::string(payload.begin(), payload.end()));
  } catch (const std::exception&) {
    throw spatial::core::ValidationError(
        spatial::core::ErrorCode::kValidationDomain,
        "host-runner commit rejected a malformed worker reconstruction "
        "artifact",
        /*details=*/{}, /*recoverable=*/false,
        "The worker CAS payload did not parse as a canonical reconstruction "
        "document; nothing was committed.");
  }

  if (rec.reconstruction_id.empty()) {
    throw spatial::core::ValidationError(
        spatial::core::ErrorCode::kValidationDomain,
        "host-runner commit rejected a reconstruction artifact with no "
        "reconstruction_id",
        /*details=*/{}, /*recoverable=*/false,
        "A committed reconstruction must name a stable identity.");
  }
  if (rec.status != "succeeded") {
    throw spatial::core::ValidationError(
        spatial::core::ErrorCode::kValidationDomain,
        "host-runner commit rejected a non-succeeded provisional reconstruction"
        " artifact",
        /*details=*/{}, /*recoverable=*/false,
        "A provisional reconstruction with status \"" + rec.status +
            "\" cannot be materialized as a succeeded revision.");
  }

  spatial::core::ReconstructionRow row;
  row.reconstruction_id = spatial::core::ParseUuid(rec.reconstruction_id);
  row.scene_id = scene_id;
  row.coordinate_frame = rec.coordinate_frame;
  row.status = "succeeded";
  row.created_at_ns = created_at_ns;
  row.document_json = spatial::core::ReconstructionToJson(rec);
  db.AddReconstruction(row);
  return rec;
}

}  // namespace spatial::engine