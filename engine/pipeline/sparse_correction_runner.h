#pragma once

// P3.1 — the in-process HOST-RUNNER for the p3_sparse_correction pipeline
// (docs/architecture/P3.1-production-sparse-correction-closure.md §4 P-2/P-3).
// The full corrected chain (review re-open; §40-directed scope):
//
//   worker/latest sparse reconstruction (CAS)
//     -> [corrected chain, OPTIONAL via run config]
//        loop-closure geometric verification (matcher + verifier seams)
//        -> metric loop-closure -> PoseGraph (INV-3 gate)
//        -> TrajectoryOptimizer seam -> ApplyOptimizedTrajectory (pose feedback)
//        -> committed v2 revision
//     -> re-triangulation v3 revision (FROZEN 8b DLT, resolved observations)
//     -> BundleAdjustmentOptimizePipeline v4 (seam, D5 gate, P14 ordering)
//     -> canonical v4 document (CAS) + revision lifecycle rows
//
// The engine runs the pipeline through a single InProcessExecutor whose runner
// dispatches by task_type:
//   * "sparse_reconstruction" — the worker-compute segment. Images mode drives
//     a ProcessExecutor + colmap_worker subprocess over the shared CAS
//     (composition-root wiring: engine/pipeline never spawns workers itself);
//     reconstruction mode forwards the canonical reconstruction document. Both
//     are deterministic and replayable (ADR-020).
//   * "sparse_correction" — the host COMMIT + re-triangulation + stage-6
//     segment (sparse_correction_orchestrator.h + core/geometry/triangulation.h
//     + bundle_adjustment_optimize_pipeline.h) with the injected seams.
//     DB-committing => CachePolicy::kNever (a replay would skip the writes).
//
// The host-runner is the ONLY MetadataDb writer on this path (worker boundary,
// scripts/check_worker_boundary.py). The pipeline never links a backend: the BA
// work, the trajectory optimization, and the feature matching / geometric
// verification run wholly behind the injected seams (D-AB-02).
//
// Corrected-chain run-configuration (the pipeline user's run config, wrapped
// under "config" by the PipelineCompiler). When `metric.trajectory` and
// `loop_closure` are BOTH present the LC/trajectory segment runs BEFORE the
// commit (P3.1 Step 4); when both are absent the run degrades to the plain
// commit + BA path (worker-mode passthrough). Exactly one of the two present
// is a half-configured segment and fails closed (P12).
//   metric.trajectory: {"trajectory_id", "nodes": [{"frame_id",
//                     "timestamp_ns"?, "sequence_index"?,
//                     "position_xyz"[3], "rotation_xyzw"[x,y,z,w]} (non-empty),
//       "metric_basis"?: {declared!, type?, source?, scale_calibration_ref?,
//                        provenance?:{configuration_hash}}}  // D4 / INV-3
//   loop_closure: {"candidate": {"candidate_id", "trajectory_id",
//                     "source_frame_id", "target_frame_id", "feature_match_score",
//                     "matcher", "created_at_ns"?},
//                  "source_frame": {"frame_id", "timestamp_ns",
//                     "feature_artifact_uuid"},       // VerificationFrameInput
//                  "target_frame": {...}}
//   feature_artifacts?: {"<frame_id>": "<feature_artifact_uuid>"}  // 2D obs
//     (fallback for the artifact uuid only; the frame blocks are primary)
// Per-frame ReconCamera is resolved from the input reconstruction document
// itself (frame_id -> ReconImage -> camera_id -> ReconCamera, D-CRM-04/05/20),
// never from config_json and never from a runner-side store. A by-fiat basis
// (declared without evidence) is not an error: it yields verified-visual-only
// (INV-3). The metric closure (if any) is observed via loop_closures rows +
// the loop-closure CAS payload; PoseGraph/GTSAM consumption is a later step.
//
// All corrected-chain seam dependencies must be injected; a missing seam fails
// closed BEFORE any database write (P12). The run ALWAYS checks the scene's
// existing latest revision status BEFORE committing (N6: only a succeeded
// latest may be superseded by the correction).

#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/geometry/reconstruction_optimizer.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/loop_closure/geometric_verifier.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/optimizer.h"
#include "engine/workers/in_process_executor.h"

namespace spatial::engine {

// Seams the production entry point (CLI, tests) injects; the engine owns no
// optimizer implementation and never includes a backend type. The corrected
// chain requires trajectory_optimizer + matcher + verifier; the plain commit +
// BA path requires only ba_optimizer.
struct SparseCorrectionSeams {
  spatial::core::geometry::ReconstructionOptimizer* ba_optimizer = nullptr;
  spatial::core::TrajectoryOptimizer* trajectory_optimizer = nullptr;
  spatial::core::LoopClosureFeatureMatcher* matcher = nullptr;
  spatial::core::GeometricVerifier* verifier = nullptr;
};

// Profile the pipeline executor advertises so the compiler's capability
// binding (ADR-011/034) accepts both declared stage capabilities. Capability
// names come from the taxonomy enum (schemas/json/worker-capabilities.schema
// .json) — the correction stage reuses "bundle_adjustment" because extending
// the taxonomy requires an RFC.
ResourceProfile SparseCorrectionProfile();

// Builds the in-process host runner. `worker_command` is the colmap_worker +
// probe-shim argv for images mode (empty => reconstruction passthrough only).
// Lives for the lifetime of the Engine; `db`/`store` must outlive it.
InProcessTaskRunner MakeSparseCorrectionRunner(
    spatial::core::MetadataDb& db, spatial::core::ArtifactStore& store,
    SparseCorrectionSeams seams = {},
    std::vector<std::string> worker_command = {});

}  // namespace spatial::engine