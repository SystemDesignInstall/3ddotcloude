// P3-impl-7 Phase 3 — orchestration-runner stage: persist + optimize pipeline
// (P3-impl-7c §7.2 persistence increment).
//
// See loop_closure_optimize_pipeline.h for the exact contract. This stage wires
// the previously proven chain into the real engine orchestration path and
// persists the canonical rows (PoseGraph / LoopClosure / OptimizationResult)
// idempotently, running the optimizer ONLY through the injected
// core::TrajectoryOptimizer seam. No GTSAM, no raw Eigen, no new domain-type
// debt.

#include "engine/pipeline/loop_closure_optimize_pipeline.h"

#include <array>
#include <cstdint>
#include <string>

#include "core/storage/metadata_db.h"
#include "core/trajectory/optimization.h"
#include "core/trajectory/optimizer.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/pose_graph_helpers.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/uuid.h"

namespace spatial::engine {

namespace {

// Fixed namespace (RFC 4122) for deterministic v5 identities derived from
// (trajectory_id, closure_id). This makes graph_id / result_id STABLE across
// re-processing of the same verified closure, so Upsert* overwrites the same
// canonical rows instead of creating duplicates or contradictions.
constexpr std::uint8_t kOrchestrationNamespace[16] = {
    0x6b, 0xa7, 0xc9, 0x01, 0x9d, 0xad, 0x11, 0xd1,
    0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc9};

spatial::core::Uuid NameUuid(const std::string& name) {
  return spatial::core::GenerateUuidV5(
      spatial::core::Uuid{kOrchestrationNamespace[0], kOrchestrationNamespace[1],
                          kOrchestrationNamespace[2], kOrchestrationNamespace[3],
                          kOrchestrationNamespace[4], kOrchestrationNamespace[5],
                          kOrchestrationNamespace[6], kOrchestrationNamespace[7],
                          kOrchestrationNamespace[8], kOrchestrationNamespace[9],
                          kOrchestrationNamespace[10], kOrchestrationNamespace[11],
                          kOrchestrationNamespace[12], kOrchestrationNamespace[13],
                          kOrchestrationNamespace[14], kOrchestrationNamespace[15]},
      name);
}

// Maps a canonical LoopClosure to its DB row, with the D2 spatial_separation_m
// diagnostic (recomputed from the trajectory nodes) filled.
spatial::core::LoopClosureRow ToLoopClosureRow(const spatial::core::LoopClosure& c,
                                               double spatial_separation_m) {
  spatial::core::LoopClosureRow row;
  row.closure_id = spatial::core::ParseUuid(c.closure_id);
  row.trajectory_id = spatial::core::ParseUuid(c.trajectory_id);
  row.candidate_id = c.candidate_id.empty()
                         ? spatial::core::Uuid{}
                         : spatial::core::ParseUuid(c.candidate_id);
  row.source_frame_id = spatial::core::ParseUuid(c.source_frame_id);
  row.target_frame_id = spatial::core::ParseUuid(c.target_frame_id);
  row.status = c.status;
  row.inlier_ratio = c.inlier_ratio;
  row.inlier_count = c.inlier_count;
  row.confidence = c.confidence;
  row.temporal_separation_ns = c.temporal_separation_ns;
  row.spatial_separation_m = spatial_separation_m;
  row.created_at_ns = c.created_at_ns;
  return row;
}

spatial::core::PoseGraphRow ToPoseGraphRow(const spatial::core::PoseGraph& g) {
  spatial::core::PoseGraphRow row;
  row.graph_id = spatial::core::ParseUuid(g.graph_id);
  row.trajectory_id = spatial::core::ParseUuid(g.trajectory_id);
  row.scene_id = spatial::core::ParseUuid(g.scene_id);
  row.status = g.status;
  row.node_count = g.node_count;
  row.edge_count = g.edge_count;
  row.odometry_edge_count = g.odometry_edge_count;
  row.loop_closure_edge_count = g.loop_closure_edge_count;
  row.prior_edge_count = g.prior_edge_count;
  row.created_at_ns = g.created_at_ns;
  row.document_json = g.graph_id;  // full graph document lives in CAS; identity placeholder
  return row;
}

spatial::core::OptimizationResultRow ToOptimizationResultRow(
    const spatial::core::OptimizationResult& r) {
  spatial::core::OptimizationResultRow row;
  row.result_id = spatial::core::ParseUuid(r.result_id);
  row.graph_id = spatial::core::ParseUuid(r.graph_id);
  row.trajectory_id = spatial::core::ParseUuid(r.trajectory_id);
  row.status = r.status;
  row.iterations = r.iterations;
  row.initial_error = r.initial_error;
  row.final_error = r.final_error;
  row.error_reduction = r.error_reduction;
  row.created_at_ns = r.created_at_ns;
  row.document_json = r.result_id;  // full document lives in CAS; identity placeholder
  return row;
}

}  // namespace

LoopClosureOptimizePipelineResult LoopClosureOptimizePipeline(
    const LoopClosureOptimizePipelineInput& input) {
  LoopClosureOptimizePipelineResult out;
  if (input.trajectory == nullptr || input.trajectory_nodes == nullptr) return out;

  // 1. Run the LoopClosure -> PoseGraph stage: metric edge + D2
  //    spatial_separation_m (from the trajectory nodes).
  LoopClosureToPoseGraphInput stage_in;
  stage_in.trajectory = input.trajectory;
  stage_in.trajectory_nodes = input.trajectory_nodes;
  stage_in.closure = input.closure;
  stage_in.metric_basis = input.metric_basis;
  stage_in.configuration_hash = input.configuration_hash;
  const LoopClosureToPoseGraphResult staged = LoopClosureToPoseGraph(stage_in);

  out.spatial_separation_m = staged.spatial_separation_m;
  out.metric_eligible = staged.metric_eligible;

  // 2. Persist the closure row idempotently (real UPDATE path for
  //    spatial_separation_m). Always done for accepted/rejected closures.
  if (input.db != nullptr) {
    input.db->UpsertLoopClosure(ToLoopClosureRow(staged.closure,
                                                 staged.spatial_separation_m));
  }

  // Fail closed on the metric path when dependencies are missing.
  if (input.optimizer == nullptr || input.db == nullptr) return out;
  if (!staged.metric_edge.has_value()) return out;  // INV-1 / INV-3: no edge, no optimize

  // 3. Assemble the FULL PoseGraph: odometry edges + the one metric loop edge.
  spatial::core::PoseGraphAssemblerOptions aopts;
  aopts.odometry_info_position = input.odometry_info_position;
  aopts.odometry_info_rotation = input.odometry_info_rotation;
  spatial::core::PoseGraphAssembly assembly =
      spatial::core::AssemblePoseGraph(*input.trajectory, *input.trajectory_nodes,
                                       aopts);

  // Stable, idempotent graph identity derived from (trajectory_id, closure_id).
  const std::string graph_id =
      spatial::core::FormatUuid(NameUuid("graph|" + input.closure.trajectory_id +
                                         "|" + input.closure.closure_id));
  assembly.graph.graph_id = graph_id;

  // Re-stamp the metric edge with a deterministic edge id and append it.
  spatial::core::PoseGraphEdge metric_edge = *staged.metric_edge;
  metric_edge.edge_id = assembly.graph_edges.size();
  assembly.graph_edges.push_back(metric_edge);
  assembly.graph.edge_count = static_cast<std::int64_t>(assembly.graph_edges.size());
  assembly.graph.odometry_edge_count =
      static_cast<std::int64_t>(assembly.graph_edges.size() - 1);
  assembly.graph.loop_closure_edge_count = 1;

  out.graph = assembly.graph;
  out.graph_id = graph_id;

  // Persist the PoseGraph row idempotently.
  input.db->UpsertPoseGraph(ToPoseGraphRow(assembly.graph));

  // 4. Run the optimizer through the seam.
  spatial::core::PoseOptimizationInput opt_in;
  opt_in.graph = assembly.graph;
  opt_in.graph_nodes = assembly.graph_nodes;
  opt_in.graph_edges = assembly.graph_edges;
  opt_in.initial_nodes = *input.trajectory_nodes;
  opt_in.anchor_enabled = true;
  const spatial::core::PoseOptimizationOutput seam_out =
      input.optimizer->optimize(opt_in);

  // 5. Stable, idempotent result identity (state that GTSAM ran over graph_id).
  const std::string result_id =
      spatial::core::FormatUuid(NameUuid("result|" + graph_id));

  spatial::core::OptimizationResult opt_result;
  opt_result.result_id = result_id;
  opt_result.graph_id = graph_id;
  opt_result.trajectory_id = input.closure.trajectory_id;
  opt_result.status = seam_out.trace.status;
  opt_result.iterations = seam_out.trace.iterations;
  opt_result.initial_error = seam_out.trace.initial_error;
  opt_result.final_error = seam_out.trace.final_error;
  opt_result.error_reduction = seam_out.trace.error_reduction;
  opt_result.created_at_ns = input.closure.created_at_ns;
  opt_result.provenance.optimizer.name = "gtsam";  // backend surfaced via the seam
  opt_result.provenance.optimizer.version = "seam";
  opt_result.provenance.adapter_version = "0.1.0";

  // Persist the OptimizationResult row idempotently (stable identity).
  input.db->UpsertOptimizationResult(ToOptimizationResultRow(opt_result));

  // 6. Map the seam's corrected nodes to the optimizer-typed consumer output.
  out.optimized_nodes = seam_out.optimized_nodes;
  out.optimized.reserve(seam_out.optimized_nodes.size());
  for (const auto& n : seam_out.optimized_nodes) {
    spatial::core::OptimizedPoseNode o;
    o.frame_id = n.frame_id;
    o.timestamp_ns = n.timestamp_ns;
    o.sequence_index = n.sequence_index;
    o.position_xyz = n.position_xyz;
    o.rotation_xyzw = n.rotation_xyzw;
    o.covariance_position = n.covariance_position;
    o.covariance_rotation = n.covariance_rotation;
    out.optimized.push_back(std::move(o));
  }
  out.optimization = opt_result;
  out.result_id = result_id;
  out.ran = true;
  return out;
}

}  // namespace spatial::engine
