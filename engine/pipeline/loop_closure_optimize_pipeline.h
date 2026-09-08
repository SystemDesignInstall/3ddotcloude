#pragma once

// P3-impl-7 Phase 3 (P3-impl-7c §7.2) — the orchestration-runner stage that
// assembles the persisted, metric-optimized trajectory pipeline:
//
//   Verified LoopClosure (7b, D5 resolved pose)
//     -> loop_closure_to_pose_graph (metric edge + D2 spatial_separation_m)
//     -> PoseGraph (odometry edges + ONE metric loop edge)
//     -> DB: UpsertPoseGraph (idempotent PoseGraphRow)
//     -> DB: UpsertLoopClosure  (idempotent UPDATE of spatial_separation_m)
//     -> TrajectoryOptimizer seam (injected) -> GTSAM (behind the seam)
//     -> DB: UpsertOptimizationResult (idempotent OptimizationResultRow)
//
// Responsibility boundaries (D-AB-02, P3-impl-7c §7.1):
//   - This stage NEVER #includes GTSAM or any optimizer backend. The optimizer
//     is dependency-injected through the core::TrajectoryOptimizer seam, so the
//     engine owns neither the optimizer implementation nor GTSAM types.
//   - The stage produces the canonical OptimizationResult + optimized nodes the
//     caller feeds to ApplyOptimizedTrajectory (the consumer seam, whose DB
//     supersede path is exercised by the INV-4 orchestration test).
//   - Idempotency: every persisted canonical row carries a STABLE identity
//     (graph_id = f(trajectory_id, closure_id); closure_id is the closure's own
//     identity; result_id = f(trajectory_id, closure_id) so re-processing the
//     SAME verified closure overwrites the SAME rows instead of creating
//     duplicate/contradictory canonical rows). Persistence uses INSERT OR
//     REPLACE (Upsert*), never raw INSERT, on this path.

#include <optional>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/optimization.h"
#include "core/trajectory/optimizer.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/trajectory.h"
#include "engine/pipeline/loop_closure_to_pose_graph.h"

namespace spatial::engine {

// Inputs to the orchestration-runner stage.
struct LoopClosureOptimizePipelineInput {
  const spatial::core::Trajectory* trajectory = nullptr;
  const std::vector<spatial::core::TrajectoryPoseNode>* trajectory_nodes = nullptr;
  spatial::core::LoopClosure closure;      // accepted 7b result (D5 fields set)
  spatial::core::MetricBasis metric_basis; // D4, from the trajectory root
  std::string configuration_hash;          // forwarded to produced edges
  spatial::core::TrajectoryOptimizer* optimizer = nullptr;  // seam (engine never sees GTSAM)
  spatial::core::MetadataDb* db = nullptr;                  // persistence
  spatial::core::ArtifactStore* store = nullptr;            // CAS persistence (§3.3)
  double odometry_info_position = 100.0;   // assemble odometry-edges info block
  double odometry_info_rotation = 100.0;
};

// Outcome of the stage, for the caller to continue into the reconstruction
// consumer (ApplyOptimizedTrajectory).
struct LoopClosureOptimizePipelineResult {
  bool ran = false;                        // true when an optimization was executed
  double spatial_separation_m = 0.0;       // D2 diagnostic (also persisted on the closure row)
  bool metric_eligible = false;            // INV-3 gate
  spatial::core::PoseGraph graph;          // assembled graph (odometry + metric edge), if any
  std::string graph_id;                    // stable idempotent identity
  std::optional<spatial::core::OptimizationResult> optimization;
  std::vector<spatial::core::TrajectoryPoseNode> optimized_nodes;  // seam output (corrected poses)
  std::vector<spatial::core::OptimizedPoseNode> optimized;         // optimizer-typed, for consumer seam
  std::string result_id;                   // stable idempotent identity
};

// Runs the orchestration-runner stage.
//
// Behaviour:
//   - Always persists the accepted/rejected closure row via UpsertLoopClosure,
//     so its spatial_separation_m (recomputed from the trajectory nodes) is
//     written through a real UPDATE path (INSERT OR REPLACE) — never lost.
//   - When the closure is metric-eligible and a real metric edge is produced:
//       * assembles the full PoseGraph (odometry edges + the metric loop edge),
//       * persists the PoseGraphRow (UpsertPoseGraph, stable graph_id),
//       * runs the injected seam optimizer (TrajectoryOptimizer) over the graph,
//       * persists the OptimizationResultRow (UpsertOptimizationResult, stable
//         result_id), and returns the corrected nodes for reconstruction.
//   - When not metric-eligible (INV-3) or no metric measurement (INV-1), NO
//     pose graph edge, NO optimization: ran=false; the closure remains
//     verified-visual-only but is still persisted (audit).
//
// The stage NEVER throws for a valid closure; a missing optimizer or db
// dependency yields ran=false (fail closed on the metric path).
LoopClosureOptimizePipelineResult LoopClosureOptimizePipeline(
    const LoopClosureOptimizePipelineInput& input);

}  // namespace spatial::engine
