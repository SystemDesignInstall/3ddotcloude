#pragma once

// P3-impl-7 Phase 3 (P3-impl-7c §7.2): the LoopClosure -> PoseGraph pipeline
// stage.
//
// Contract:
//   Accepted LoopClosure (7b, D5 fields populated) + Trajectory + pose nodes
//     -> resolve source/target frames -> trajectory pose nodes (frame_id bridge)
//     -> compute spatial_separation_m = ||p_t_ww - p_s_ww|| (D2, diagnostic)
//     -> metric-eligibility check (trajectory metric_basis, D4 / INV-3)
//     -> eligible  -> BuildMetricLoopClosureEdge (ONE PoseGraphEdge, real
//                     metric relative pose + deterministic D3 info matrix)
//     -> ineligible -> NO edge (closure persists as verified-visual only)
//
// This stage is the SINGLE consumer of the metric branch (P3-impl-7c §10) and
// the home of the D2 spatial_separation_m fix (Phase-3 spec §5, T5). It never
// #includes GTSAM; the optimizer is dispatched through the TrajectoryOptimizer
// seam (core/trajectory/optimizer.h) by the caller.

#include <optional>
#include <string>
#include <vector>

#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/trajectory.h"

namespace spatial::engine {

// Inputs to the loop_closure_to_pose_graph stage.
struct LoopClosureToPoseGraphInput {
  const spatial::core::Trajectory* trajectory = nullptr;
  const std::vector<spatial::core::TrajectoryPoseNode>* trajectory_nodes = nullptr;
  spatial::core::LoopClosure closure;      // accepted 7b result (D5 fields set)
  spatial::core::MetricBasis metric_basis; // D4, from the trajectory root
  std::string configuration_hash;          // forwarded to the produced edge
};

// Output of the stage.
struct LoopClosureToPoseGraphResult {
  spatial::core::LoopClosure closure;      // same instance identity, spatial_separation_m filled (D2)
  double spatial_separation_m = 0.0;       // ||p_t_ww - p_s_ww|| in W (diagnostic, D2)
  bool metric_eligible = false;            // trajectory metric_basis declared + valid (INV-3)
  std::optional<spatial::core::PoseGraphEdge> metric_edge;  // present iff eligible + measurement valid
};

// Runs the stage. NEVER throws for a valid accepted/rejected closure; the
// frame-resolution baseline and measured geometry determine the outcome.
//
// The produced metric_edge, when present, is the ONLY edge type this stage
// emits; it is a REAL metric relative-pose constraint with the D3
// deterministic information matrix (never the fabricated 6c identity, INV-1).
// When the trajectory is not metric-eligible (undeclared / by-fiat metric
// basis), or the measurement is absent, or the consistency/D1/INV-1 guards
// reject it, no edge is produced (INV-3) and the closure remains
// verified-visual-only (persistable, non-constraining).
LoopClosureToPoseGraphResult LoopClosureToPoseGraph(
    const LoopClosureToPoseGraphInput& input);

}  // namespace spatial::engine
