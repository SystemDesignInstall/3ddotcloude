// P3-impl-8c engine orchestration stage (P11/P14/P16; docs/architecture/
// P3-impl-8c-bundle-adjustment-readiness.md §5). See the header for the exact
// contract. This stage wires the injected core::geometry::ReconstructionOptimizer
// seam into the governed v3 -> v4 revision step with the P14 ordering:
// AddReconstruction(v4) FIRST, then SetReconstructionStatus(v3, "superseded").
//
// The stage never touches COLMAP types, workspaces, or subprocesses — the
// backend runs wholly behind the seam (P2). All writes go through MetadataDb
// in strict order, and every precondition is checked BEFORE the first write so
// a failing stage cannot leave a half-persisted revision (P12: no partial
// results).

#include "engine/pipeline/bundle_adjustment_optimize_pipeline.h"

#include <cmath>
#include <sstream>
#include <string>

#include "core/errors/project_error.h"
#include "core/geometry/reprojection.h"
#include "core/reconstruction/reconstruction_json.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"

namespace spatial::engine {

namespace {

std::string FormatNumber(double value) {
  std::ostringstream out;
  out.precision(6);
  out << value;
  return out.str();
}

void ThrowMissingSeed() {
  throw spatial::core::ValidationError(
      spatial::core::ErrorCode::kValidationDomain,
      "bundle-adjustment orchestration requires a pinned non-empty random_seed "
      "(D6)",
      /*details=*/{}, /*recoverable=*/false,
      "Set BundleAdjustmentOptimizePipelineInput.random_seed from the "
      "effective configuration; the same input + same seed must give the same "
      "derived metrics.");
}

}  // namespace

BundleAdjustmentOptimizePipelineResult BundleAdjustmentOptimizePipeline(
    const BundleAdjustmentOptimizePipelineInput& input) {
  BundleAdjustmentOptimizePipelineResult out;

  // Fail closed on missing dependencies BEFORE reading anything (mirrors the
  // loop-closure orchestration stage).
  if (input.optimizer == nullptr) {
    out.failure = "bundle-adjustment pipeline missing optimizer (ReconstructionOptimizer seam)";
    return out;
  }
  if (input.db == nullptr) {
    out.failure = "bundle-adjustment pipeline missing MetadataDb";
    return out;
  }
  if (input.source_v3 == nullptr) {
    out.failure = "bundle-adjustment pipeline missing source reconstruction (v3)";
    return out;
  }
  if (!input.random_seed || input.random_seed->empty()) ThrowMissingSeed();

  // Precondition checks BEFORE any write (P14 no-half-persist): the v3 must be
  // the succeeded revision the engine selected. A non-"succeeded" source would
  // make the later "succeeded" -> "superseded" transition throw AFTER the v4
  // insert, leaving two active revisions.
  if (input.source_v3->status != "succeeded") {
    throw spatial::core::ValidationError(
        spatial::core::ErrorCode::kValidationDomain,
        "bundle adjustment source must be a succeeded v3 revision (got status '" +
            input.source_v3->status + "')",
        /*details=*/{}, /*recoverable=*/false,
        "Only the latest succeeded reconstruction (QueryLatestReconstructionByScene "
        "with status 'succeeded') may feed the bundle-adjustment stage.");
  }

  // 1. Run the seam (v3 + observations + pinned seed -> v4 document + trace).
  spatial::core::geometry::BundleAdjustmentInput seam_input;
  seam_input.source = *input.source_v3;
  seam_input.observations = input.observations;
  seam_input.random_seed = input.random_seed;
  seam_input.min_parallax_deg = input.min_parallax_deg;
  seam_input.max_iterations = input.max_iterations;
  const spatial::core::geometry::BundleAdjustmentResult seam =
      input.optimizer->optimize(seam_input);

  // D-CRM-07: the v4 identity MUST be fresh UUIDv4 (never content-derived, and
  // never the v3 id — equality would corrupt the revision chain). Fail closed.
  if (seam.reconstruction.reconstruction_id == input.source_v3->reconstruction_id) {
    throw spatial::core::ValidationError(
        spatial::core::ErrorCode::kValidationDomain,
        "bundle-adjustment seam returned the v3 reconstruction id; the v4 "
        "document must carry a fresh UUIDv4 (D-CRM-07)",
        /*details=*/{}, /*recoverable=*/false,
        "Fix the seam (ReconstructionOptimizer) to emit a fresh UUID for the "
        "v4 identity; the engine never derives reconstruction identities.");
  }

  out.ran = true;
  out.trace = seam.trace;
  out.v4 = seam.reconstruction;

  // 2. D5 acceptance gate (strict 0.9; RMS_after < 0.9 * RMS_before). A failed
  //    gate means NO v4 insert and NO supersede — v3 stays the succeeded
  //    revision, and the failure is reported with both RMS values (§4.5).
  out.gate_passed = spatial::core::geometry::PassesReprojectionGate(
      seam.trace.rms_after_px, seam.trace.rms_before_px,
      /*improvement_factor=*/0.9);
  if (!out.gate_passed) {
    out.failure =
        "bundle adjustment rejected by the D5 gate: rms_after " +
        FormatNumber(seam.trace.rms_after_px) + " px is not strictly below 0.9 * " +
        "rms_before " + FormatNumber(seam.trace.rms_before_px) +
        " px (outliers after " +
        FormatNumber(static_cast<double>(seam.trace.outlier_count_after)) + ")";
    return out;
  }

  // 3. Persist the v4 row (document = full canonical JSON, CAS payload adapters
  //    reconcile the row with its document by reconstruction_id), then supersede
  //    the v3 — the exact P14 ordering. Both writes happen inside ONE SQLite
  //    transaction (P12 no-partial-results): a failure between the insert and
  //    the status flip rolls the whole revision step back instead of leaving a
  //    dangling active v4 beside a still-succeeded v3.
  spatial::core::ReconstructionRow row;
  row.reconstruction_id =
      spatial::core::ParseUuid(seam.reconstruction.reconstruction_id);
  row.scene_id = input.scene_id;
  row.coordinate_frame = seam.reconstruction.coordinate_frame;
  row.status = "succeeded";
  row.created_at_ns = seam.reconstruction.created_at_ns;
  row.document_json = spatial::core::ReconstructionToJson(seam.reconstruction);
  {
    spatial::core::MetadataDb::WriteTransaction txn(*input.db);
    input.db->AddReconstruction(row);
    out.inserted_v4 = true;
    input.db->SetReconstructionStatus(
        spatial::core::ParseUuid(input.source_v3->reconstruction_id),
        "superseded");
    out.superseded_v3 = true;
    txn.Commit();
  }

  return out;
}

}  // namespace spatial::engine