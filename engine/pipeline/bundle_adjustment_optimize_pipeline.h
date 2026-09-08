#pragma once

// P3-impl-8c engine orchestration stage (P11/P14/P16; docs/architecture/
// P3-impl-8c-bundle-adjustment-readiness.md §5). The DB-facing wrapper that
// turns "run the injected Bundle Adjustment seam" into the governed v3 -> v4
// revision step:
//
//   1. run the seam (v3 row + resolved observations + pinned-seed config) -> v4
//   2. enforce the D5 acceptance gate (PassesReprojectionGate, strict 0.9)
//   3. gate PASS -> AddReconstruction(v4) THEN SetReconstructionStatus(v3,
//      "superseded") (exact P14 ordering); exactly one succeeded revision per
//      scene; QueryLatestReconstructionByScene repoints implicitly.
//   4. gate FAIL -> NO v4 insert, NO supersede; the run is reported failed
//      with both RMS values + outlier counts; v3 stays the succeeded revision.
//
// The stage is pure: no COLMAP type, no workspace, no subprocess — the seam is
// injected as core::geometry::ReconstructionOptimizer (the engine never sees
// the adapter, P2). Missing optimizer/db -> ran=false, failure explained
// (mirrors loop_closure_optimize_pipeline.h). All writes go through
// MetadataDb in strict order; a typed ProjectError from the seam or a failed
// transition propagates (ADR-014) and nothing is half-persisted.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/geometry/reconstruction_optimizer.h"
#include "core/reconstruction/reconstruction.h"
#include "core/utils/uuid.h"

namespace spatial::core {
class MetadataDb;
}

namespace spatial::engine {

// Inputs to the bundle-adjustment orchestration-runner stage (P16).
struct BundleAdjustmentOptimizePipelineInput {
  const spatial::core::Reconstruction* source_v3 = nullptr;  // latest succeeded v3, immutable
  std::vector<spatial::core::geometry::ReprojectionObservation> observations;
  spatial::core::geometry::ReconstructionOptimizer* optimizer = nullptr;  // seam
  spatial::core::MetadataDb* db = nullptr;                     // persistence
  spatial::core::Uuid scene_id{};                              // row scene scope
  std::optional<std::string> random_seed;                      // pinned (D6); nullopt fails the seam closed
  double min_parallax_deg = 2.0;                               // passthrough guard (8b reuse)
  int max_iterations = 100;
};

// Outcome of the stage. `ran` distinguishes "missing deps refused to run"
// (fail closed on the seam/db dependency, mirroring the loop-closure stage)
// from a real run. `gate_passed`, `inserted_v4` and `trace` let callers emit
// the P11 failed-run report and continue into downstream consumers.
struct BundleAdjustmentOptimizePipelineResult {
  bool ran = false;
  bool gate_passed = false;
  bool inserted_v4 = false;
  bool superseded_v3 = false;
  std::optional<spatial::core::geometry::BundleAdjustmentTrace> trace;
  std::optional<spatial::core::Reconstruction> v4;
  std::string failure;      // dependency/probe diagnostics when ran == false
};

// Runs the stage with the exact P14 ordering. Throws a typed ProjectError
// when the seam fails (P12: no partial results); returns a gate-failed result
// WITHOUT any DB write when the acceptance gate rejects the v4.
BundleAdjustmentOptimizePipelineResult BundleAdjustmentOptimizePipeline(
    const BundleAdjustmentOptimizePipelineInput& input);

}  // namespace spatial::engine