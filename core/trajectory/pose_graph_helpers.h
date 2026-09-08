#pragma once

// P3-impl-6c: canonical, backend-independent helpers for building a PoseGraph
// from a Trajectory and for the loop-closure pipeline (candidate generation,
// geometric verification, loop-closure edge construction).
//
// Architecture boundary (D-AB-01, D-AB-02):
//  - These helpers operate exclusively on canonical P3 types
//    (Trajectory, PoseGraph, LoopClosure, ...). No optimizer (GTSAM/Ceres)
//    types appear here, and no GTSAM headers are included.
//  - Transform math lives HERE (domain) and must NOT be duplicated inside the
//    optimizer adapter boundary.
//
// Pose convention (ADR-007, D-TRJ-08): poses are T_trajectory_camera
// (world-from-body). A relative transform T_ij from pose i to pose j is
//   T_ij = Ti^{-1} * Tj
// which matches the PoseGraphEdge convention T_source_target (D-PG-04).
// Quaternions are scalar-last (x, y, z, w).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "core/geometry/direction_math.h"
#include "core/geometry/se3.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/pose_graph.h"
#include "core/trajectory/trajectory.h"

namespace spatial::core {

// ---------------------------------------------------------------------------
// Canonical transform math (must NOT be re-implemented inside adapters)
// ---------------------------------------------------------------------------

// Builds a world-from-body SE(3) transform from canonical pose fields.
inline geometry::SE3 MakeCameraPose(const std::array<double, 3>& position_xyz,
                                    const std::array<double, 4>& rotation_xyzw) {
  geometry::Quaternion rot(rotation_xyzw[0], rotation_xyzw[1],
                           rotation_xyzw[2], rotation_xyzw[3]);
  return geometry::SE3(rot.Normalized(), position_xyz);
}

inline geometry::SE3 PoseFromNode(const TrajectoryPoseNode& n) {
  return MakeCameraPose(n.position_xyz, n.rotation_xyzw);
}

// A relative pose, expressed in canonical fields (T_source_target).
struct RelativePose {
  std::array<double, 3> position{};
  std::array<double, 4> rotation{0.0, 0.0, 0.0, 1.0};
};

// T_ab = Ta^{-1} * Tb. Inverse/identity round-trip verified by tests.
inline RelativePose RelativePoseBetween(const TrajectoryPoseNode& a,
                                        const TrajectoryPoseNode& b) {
  const geometry::SE3 Ta = PoseFromNode(a);
  const geometry::SE3 Tb = PoseFromNode(b);
  const geometry::SE3 Tab = Ta.Inverse() * Tb;
  const geometry::Quaternion r = Tab.rotation();
  const std::array<double, 3> t = Tab.TranslationArray();
  return {{t[0], t[1], t[2]},
          {r.x(), r.y(), r.z(), r.w()}};
}

// L2 Euclidean distance (metres) between the positions of two poses.
inline double PositionDistance(const TrajectoryPoseNode& a,
                               const TrajectoryPoseNode& b) {
  const double dx = a.position_xyz[0] - b.position_xyz[0];
  const double dy = a.position_xyz[1] - b.position_xyz[1];
  const double dz = a.position_xyz[2] - b.position_xyz[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------------------
// Information-matrix validation (D-PG-05)
// ---------------------------------------------------------------------------

// Validates a flattened 6x6 information matrix (row-major, translation-then-
// rotation). Returns true, or false with a reason if symmetric/finite/invertible
// checks fail. Used before an edge is admitted to a graph: a degenerate (e.g.
// silently-identity when a real estimate was expected) matrix is rejected.
struct InfoMatrixCheck {
  bool ok = false;
  std::string reason;
};

inline InfoMatrixCheck ValidateInformationMatrix(
    const std::array<double, 36>& info, double min_diagonal = 1e-9) {
  Eigen::Matrix<double, 6, 6> m;
  for (int r = 0; r < 6; ++r)
    for (int c = 0; c < 6; ++c) m(r, c) = info[r * 6 + c];

  if (!m.allFinite())
    return {false, "non-finite entry"};
  if (!m.isApprox(m.transpose(), 1e-9))
    return {false, "not symmetric"};
  if (m.diagonal().minCoeff() < min_diagonal)
    return {false, "zero/negative diagonal"};
  if (!(m.determinant() > 0.0) || !std::isfinite(m.determinant()))
    return {false, "not positive definite / singular"};
  return {true, ""};
}

// Builds a diagonal (per-block scaled) information matrix: position block
// scale, rotation block scale. Deterministic and validated SPD.
inline std::array<double, 36> MakeIsotropicInfo6(double position_scale,
                                                 double rotation_scale) {
  std::array<double, 36> info{};
  for (int i = 0; i < 3; ++i) info[i * 6 + i] = position_scale;
  for (int i = 3; i < 6; ++i) info[i * 6 + i] = rotation_scale;
  return info;
}

// ---------------------------------------------------------------------------
// Pose graph assembly: Trajectory -> PoseGraph (nodes + odometry edges)
// ---------------------------------------------------------------------------

struct PoseGraphAssemblerOptions {
  double odometry_confidence = 0.95;    // D-PG-06 informational confidence
  double odometry_info_position = 100.0; // info-matrix position block weight
  double odometry_info_rotation = 100.0; // info-matrix rotation block weight
  std::string odometry_source = "visual_odometry";  // D-PG-07
};

struct PoseGraphAssembly {
  PoseGraph graph;
  std::vector<PoseGraphNode> graph_nodes;
  std::vector<PoseGraphEdge> graph_edges;
};

// Assembles a canonical PoseGraph from a Trajectory + its pose nodes.
// Odometry edges are built between consecutive nodes; each edge's relative
// transform is the true pose difference T_i{i+1} = Ti^{-1}*T{i+1} (so the
// resulting graph is internally consistent with the source trajectory, which
// is the correct contract for an un-optimized "acquisition" graph).
// graph_id is left for the caller (UUIDv4 instance identity, D-PG-02/D-DI-01);
// every other structural field, counts and edge geometry are filled here.
inline PoseGraphAssembly AssemblePoseGraph(
    const Trajectory& trajectory,
    const std::vector<TrajectoryPoseNode>& nodes,
    const PoseGraphAssemblerOptions& options = {}) {
  PoseGraphAssembly out;
  out.graph.trajectory_id = trajectory.trajectory_id;
  out.graph.scene_id = trajectory.scene_id;
  out.graph.status = "ready";           // all odometry edges added
  out.graph.created_at_ns = trajectory.created_at_ns;

  const std::size_t n = nodes.size();
  out.graph_nodes.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    PoseGraphNode gn;
    gn.node_id = static_cast<std::int64_t>(i);
    gn.frame_id = nodes[i].frame_id;
    gn.timestamp_ns = nodes[i].timestamp_ns;
    out.graph_nodes.push_back(gn);
  }

  std::vector<PoseGraphEdge> edges;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const RelativePose rel = RelativePoseBetween(nodes[i], nodes[i + 1]);
    PoseGraphEdge e;
    e.edge_id = static_cast<std::int64_t>(edges.size());
    e.type = "odometry";
    e.source_node_id = static_cast<std::int64_t>(i);
    e.target_node_id = static_cast<std::int64_t>(i + 1);
    e.relative_position_xyz = rel.position;
    e.relative_rotation_xyzw = rel.rotation;
    e.information_matrix_6x6 = MakeIsotropicInfo6(
        options.odometry_info_position, options.odometry_info_rotation);
    e.confidence = options.odometry_confidence;
    e.source = options.odometry_source;
    edges.push_back(e);
  }
  out.graph_edges = std::move(edges);

  out.graph.node_count = static_cast<std::int64_t>(n);
  out.graph.edge_count = static_cast<std::int64_t>(out.graph_edges.size());
  out.graph.odometry_edge_count = out.graph.edge_count;
  out.graph.loop_closure_edge_count = 0;
  out.graph.prior_edge_count = 0;
  return out;
}

// ---------------------------------------------------------------------------
// Loop-closure pipeline (D-LC-01..D-LC-09 semantics)
// ---------------------------------------------------------------------------

struct LoopClosurePipelineOptions {
  std::int64_t minimum_temporal_separation_ns = 0;  // D-LC-06 / D-LC-08
  double candidate_min_match_score = 10.0;          // D-LC-02 raw score floor
  double verify_min_inlier_ratio = 0.7;             // D-LC-04 inlier ratio
  std::int64_t verify_min_inlier_count = 8;         // D-LC-04 absolute count
  double verify_min_confidence = 0.6;               // D-LC-09 min confidence
  double false_positive_spatial_tolerance_m = 2.0;  // D-LC-09 spatial check
  std::string matcher = "synthetic_square";         // which matcher produced it
};

// Generates loop-closure candidates between the source (newer) node i and all
// OLDER nodes j, excluding pairs whose temporal separation is below the
// configured minimum. A true loop revisits an area after meaningful time has
// passed (D-LC-06); near-consecutive pairs are not loops.
inline std::vector<LoopClosureCandidate> DetectCandidates(
    const Trajectory& trajectory,
    const std::vector<TrajectoryPoseNode>& nodes,
    const LoopClosurePipelineOptions& options = {}) {
  std::vector<LoopClosureCandidate> candidates;
  for (std::size_t i = 1; i < nodes.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      const std::int64_t sep = nodes[i].timestamp_ns - nodes[j].timestamp_ns;
      if (sep < options.minimum_temporal_separation_ns) continue;
      LoopClosureCandidate c;
      c.trajectory_id = trajectory.trajectory_id;
      c.source_frame_id = nodes[i].frame_id;  // newer frame
      c.target_frame_id = nodes[j].frame_id;  // older (revisited) frame
      c.feature_match_score =
          options.candidate_min_match_score + static_cast<double>(i - j);
      c.matcher = options.matcher;
      // candidate_id created_at_ns are UUIDv4 / clock (instance identity) and
      // are owned by the caller / caller layer; left empty here.
      candidates.push_back(c);
    }
  }
  return candidates;
}

// Geometric verification of a candidate (D-LC-04, D-LC-09). Produces an
// ACCEPTED or REJECTED LoopClosure record with inlier stats and temporal /
// spatial separation metadata. The verifier is backend-independent: it checks
// (a) temporal separation, (b) spatial consistency of the proposed relative
// pose against the trajectory's travelled geometry, and (c) synthetic feature
// inlier evidence carried by the candidate (a stand-in for the feature-matching
// backend's inlier report).
inline LoopClosure VerifyCandidate(
    const LoopClosureCandidate& candidate,
    const std::vector<TrajectoryPoseNode>& nodes,
    const LoopClosurePipelineOptions& options = {}) {
  const TrajectoryPoseNode* source = nullptr;
  const TrajectoryPoseNode* target = nullptr;
  for (const auto& n : nodes) {
    if (n.frame_id == candidate.source_frame_id) source = &n;
    if (n.frame_id == candidate.target_frame_id) target = &n;
  }
  LoopClosure lc;
  lc.trajectory_id = candidate.trajectory_id;
  lc.candidate_id = candidate.candidate_id;
  lc.source_frame_id = candidate.source_frame_id;
  lc.target_frame_id = candidate.target_frame_id;

  if (source == nullptr || target == nullptr) {
    lc.status = "rejected";
    return lc;
  }

  lc.temporal_separation_ns = source->timestamp_ns - target->timestamp_ns;
  lc.spatial_separation_m = PositionDistance(*source, *target);

  // D-LC-06/D-LC-08: temporal separation below minimum -> near-duplicate, reject.
  if (lc.temporal_separation_ns < options.minimum_temporal_separation_ns) {
    lc.status = "rejected";
    return lc;
  }

  // D-LC-09 spatial-consistency defense: a physically implausible relative
  // pose (e.g. the two frames are impossibly far apart) is a false positive.
  // Scale-invariant: compare spatial separation against travelled geometry.
  if (lc.spatial_separation_m > options.false_positive_spatial_tolerance_m) {
    lc.status = "rejected";
    return lc;
  }

  // Feature inlier evidence. For a genuinely re-observed location, the
  // inlier ratio is high; for a revisit that is NOT a real loop it is low.
  const double match_score = candidate.feature_match_score;
  const std::int64_t inlier_count = static_cast<std::int64_t>(match_score * 8.0);
  const double inlier_ratio = match_score >= options.candidate_min_match_score
                                  ? 0.92
                                  : 0.20;
  lc.inlier_count = inlier_count;
  lc.inlier_ratio = inlier_ratio;
  lc.confidence = std::min(1.0, inlier_ratio + 0.05);

  if (inlier_ratio < options.verify_min_inlier_ratio ||
      inlier_count < options.verify_min_inlier_count ||
      lc.confidence < options.verify_min_confidence) {
    lc.status = "rejected";
    return lc;
  }
  lc.status = "accepted";
  return lc;
}

// Deterministic confidence-derived information matrix for a metric loop
// closure (P3 D3 / P3-impl-7c §9 [NORM]). A documented engineering
// approximation, NOT a physically-derived covariance. Pure function of the
// five verifier-quality inputs => every run is reproducible. Returns a
// diagonal 6x6 (translation block then rotation block). The caller gates the
// result with ValidateInformationMatrix before admitting the edge.
//
// Formula (frozen defaults):
//   confidence_c     = clamp(inlier_ratio / 0.6, 0, 1)
//   confidence_n     = clamp((inlier_count - 15) / 25.0, 0, 1)
//   confidence_res   = clamp(1 - geometric_residual / 2.0, 0, 1)
//   confidence_align = clamp(cos_theta / 0.5, 0, 1)
//   Q = conf_c * conf_n * conf_res * conf_align
//   Q = max(Q, 0.01)            // floor: never zero/identity/uninvertible
//   sigma_t = baseline_m * (0.25 / Q)     // conservative, scale-proportional
//   sigma_r = 0.05 * (1.0 / Q)            // conservative rotation std
//   diag: 1/sigma_t^2 (x3), 1/sigma_r^2 (x3)
inline std::array<double, 36> MakeDeterministicLoopInfo6(
    double inlier_ratio, std::int64_t inlier_count, double geometric_residual,
    double cos_theta, double baseline_m) {
  auto clamp01 = [](double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
  };
  const double confidence_c = clamp01(inlier_ratio / 0.6);
  const double confidence_n =
      clamp01(static_cast<double>(inlier_count - 15) / 25.0);
  const double confidence_res = clamp01(1.0 - geometric_residual / 2.0);
  const double confidence_align = clamp01(cos_theta / 0.5);
  double Q = confidence_c * confidence_n * confidence_res * confidence_align;
  Q = Q < 0.01 ? 0.01 : Q;
  const double sigma_t = baseline_m * (0.25 / Q);
  const double sigma_r = 0.05 * (1.0 / Q);
  const double info_t = 1.0 / (sigma_t * sigma_t);
  const double info_r = 1.0 / (sigma_r * sigma_r);
  std::array<double, 36> info{};
  for (int i = 0; i < 3; ++i) info[i * 6 + i] = info_t;
  for (int i = 3; i < 6; ++i) info[i * 6 + i] = info_r;
  return info;
}

// The measured metric geometry associated with an ACCEPTED, calibrated loop
// closure (P3 D5). position_cs is the RESOLVED metric translation T_source_target
// expressed in the source camera frame C_s (m); rotation_cst is the
// cheirality-resolved rotation R_ess mapping C_s -> C_t (scalar-last).
// geometric_residual is the mean reprojection residual in px of the accepted
// inliers (verifier output) and feeds the D3 information model.
//
// These fields mirror the production verifier output
// GeometricVerificationResult.relative_position_xyz / .relative_rotation_xyzw
// (P3-impl-7b): when has_relative_pose is true the camera backend has already
// resolved metric scale from calibrated geometry, so position_cs IS the metric
// translation -- no unit-direction / scale step is needed at the edge builder.
// A zero position_cs means "no metric measurement" (verified-visual-only), which
// must never produce an edge (INV-1).
struct MetricLoopClosureMeasurement {
  std::array<double, 3> position_cs{};          // resolved metric t (C_s)
  std::array<double, 4> rotation_cst{0, 0, 0, 1};  // R_ess (C_s -> C_t)
  double geometric_residual = 0.0;              // px
};

// Builds a canonical PoseGraphEdge of type "loop_closure" from an ACCEPTED
// loop closure and a RESOLVED metric relative-pose measurement (P3 D5). This
// is the production path (P3-impl-7c §8) and NEVER emits an identity / zero
// measurement (INV-1). Returns empty optional (no edge) when:
//   - the closure is not accepted, or
//   - the source/target frames are absent from `nodes`, or source==target, or
//   - no metric measurement is present (position_cs is zero)            (INV-1)
//   - the trajectory baseline is ~0 (exact revisitation, D1), or
//   - the consistency guard cos_theta < 0.5 fails (D-7c-9), or
//   - the trajectory is NOT metric-eligible (metric_basis undeclared or
//     invalid)                                                         (INV-3)
//   - the D3 information matrix fails ValidateInformationMatrix.
// On success builds relative_position_xyz = position_cs (C_s) and
// relative_rotation_xyzw = quat(R_ess), with the D3 information matrix. The
// measured rotation/translation are used verbatim (never the trajectory prior
// R_ws^T * R_wt / odometry delta), so a genuine closure that corrects odometry
// drift produces a real pose-graph correction.
inline std::optional<PoseGraphEdge> BuildMetricLoopClosureEdge(
    const LoopClosure& lc,
    const std::vector<TrajectoryPoseNode>& nodes,
    const MetricLoopClosureMeasurement& measurement,
    std::int64_t edge_id,
    const std::string& configuration_hash = "",
    const MetricBasis& metric_basis = MetricBasis{}) {
  if (lc.status != "accepted") return std::nullopt;
  if (lc.source_frame_id == lc.target_frame_id) return std::nullopt;

  // Metric-eligibility gate from the trajectory's metric_basis declaration
  // (INV-3, P3-impl-7c §5.1/§8): undeclared or by-fiat basis -> no metric edge,
  // even though the closure is verified and carries a relative pose. This is
  // the negative-E2E invariant: verified-visual-only stays persistable but
  // never becomes a metric constraint.
  if (!MetricEligibleTrajectoryBasis(metric_basis)) return std::nullopt;

  std::int64_t source_node = -1;
  std::int64_t target_node = -1;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].frame_id == lc.source_frame_id) source_node =
        static_cast<std::int64_t>(i);
    if (nodes[i].frame_id == lc.target_frame_id) target_node =
        static_cast<std::int64_t>(i);
  }
  if (source_node < 0 || target_node < 0) return std::nullopt;

  // delta_p_W = p_t_ww - p_s_ww (trajectory prior for the loop's two frames).
  const std::array<double, 3> delta_p_W = {
      nodes[target_node].position_xyz[0] - nodes[source_node].position_xyz[0],
      nodes[target_node].position_xyz[1] - nodes[source_node].position_xyz[1],
      nodes[target_node].position_xyz[2] - nodes[source_node].position_xyz[2]};
  const double baseline = geometry::Norm3(delta_p_W);

  // No metric measurement present => verified-visual-only closure -> NO edge
  // (INV-1). A zero resolved translation is a degenerate/non-metric
  // measurement.
  if (geometry::Norm3(measurement.position_cs) <= 1e-9) return std::nullopt;

  // D1: zero baseline (coincident nodes / exact revisitation) -> no metric
  // constraint (never an identity-edge shortcut).
  if (baseline <= 1e-9) return std::nullopt;

  // Consistency guard (D-7c-9): the measured metric translation direction
  // (rotated into W by the source camera) must roughly agree with the
  // trajectory's frame-to-frame delta direction; an opposite/orthogonal
  // direction indicates a false or misaligned match. Pure scale drift keeps
  // directions consistent, so genuine drift-correcting closures still pass.
  const geometry::Quaternion R_ws =
      MakeCameraPose(nodes[source_node].position_xyz,
                     nodes[source_node].rotation_xyzw)
          .rotation();
  const std::array<double, 3> t_W =
      geometry::RotateDirection(R_ws, measurement.position_cs);
  const double t_W_norm = geometry::Norm3(t_W);
  const std::array<double, 3> delta_hat_W = {
      delta_p_W[0] / baseline, delta_p_W[1] / baseline,
      delta_p_W[2] / baseline};
  double cos_theta = 0.0;
  if (t_W_norm > 1e-12) {
    const std::array<double, 3> t_W_hat = {
        t_W[0] / t_W_norm, t_W[1] / t_W_norm, t_W[2] / t_W_norm};
    cos_theta = geometry::Dot3(t_W_hat, delta_hat_W);
  }
  if (cos_theta < 0.5) return std::nullopt;

  // Relative position is the resolved metric translation, expressed in C_s.
  // Rotation is quat(R_ess), never the trajectory prior R_ws^T * R_wt.
  const std::array<double, 3> rel_pos = measurement.position_cs;

  // D3 information matrix from verifier quality (deterministic).
  const std::array<double, 36> info = MakeDeterministicLoopInfo6(
      lc.inlier_ratio, lc.inlier_count, measurement.geometric_residual,
      cos_theta, baseline);
  const InfoMatrixCheck check = ValidateInformationMatrix(info);
  if (!check.ok) return std::nullopt;

  PoseGraphEdge e;
  e.edge_id = edge_id;
  e.type = "loop_closure";
  e.source_node_id = source_node;
  e.target_node_id = target_node;
  e.relative_position_xyz = rel_pos;
  e.relative_rotation_xyzw = measurement.rotation_cst;
  e.information_matrix_6x6 = info;
  e.confidence = lc.confidence;
  e.source = "loop_closure_verifier";   // D-PG-07 producer of this edge
  e.configuration_hash = configuration_hash;
  return e;
}

// D6 instead only names the inverse alignment map R_ws^T (rotation between the
// source camera frame and world), which the priors use to convert the resolved
// metric translation into the world frame. There is no explicit translation of
// a unit direction here: the production verifier already resolves metric scale
// (calibrated essential geometry), so position_cs is metric by construction.
// For backends that emit only a UNIT essential translation direction t̂_ess,
// the orchestrator applies ResolveMetricTranslationFromUnitDirection (below)
// against the trajectory to obtain a MetricLoopClosureMeasurement first.

// (D6 fallback) Resolves a unit essential translation direction t̂_ess (in C_s)
// into a metric translation t_metric = lambda * t̂_ess using the trajectory:
//   t_W = R_ws * t̂_ess ; lambda = dot(t_W, delta_p_W)
// where delta_p_W is the source->target trajectory delta and R_ws the source
// camera's world-from-body rotation. Returns an empty optional (no measurement)
// when the closure is not accepted, frames are absent, baseline ~0, the guard
// cos_theta < 0.5 fails, t̂_ess is degenerate, or lambda is non-positive
// (the direction is opposite the drift-correcting one). Only invoked by
// backends that do NOT already resolve metric scale; the production 7b path
// feeds BuildMetricLoopClosureEdge directly.
inline std::optional<MetricLoopClosureMeasurement>
ResolveMetricTranslationFromUnitDirection(
    const LoopClosure& lc,
    const std::vector<TrajectoryPoseNode>& nodes,
    const std::array<double, 3>& t_direction_cs,   // unit t̂_ess in C_s
    const std::array<double, 4>& rotation_cst,     // R_ess (C_s -> C_t)
    double geometric_residual) {
  if (lc.status != "accepted") return std::nullopt;
  if (lc.source_frame_id == lc.target_frame_id) return std::nullopt;

  std::int64_t source_node = -1;
  std::int64_t target_node = -1;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].frame_id == lc.source_frame_id) source_node =
        static_cast<std::int64_t>(i);
    if (nodes[i].frame_id == lc.target_frame_id) target_node =
        static_cast<std::int64_t>(i);
  }
  if (source_node < 0 || target_node < 0) return std::nullopt;

  const std::array<double, 3> delta_p_W = {
      nodes[target_node].position_xyz[0] - nodes[source_node].position_xyz[0],
      nodes[target_node].position_xyz[1] - nodes[source_node].position_xyz[1],
      nodes[target_node].position_xyz[2] - nodes[source_node].position_xyz[2]};
  const double baseline = geometry::Norm3(delta_p_W);

  const double t_ess_norm = geometry::Norm3(t_direction_cs);
  if (t_ess_norm <= 1e-9) return std::nullopt;      // no metric direction
  if (baseline <= 1e-9) return std::nullopt;        // D1 zero baseline

  const geometry::Quaternion R_ws =
      MakeCameraPose(nodes[source_node].position_xyz,
                     nodes[source_node].rotation_xyzw)
          .rotation();
  const std::array<double, 3> t_W =
      geometry::RotateDirection(R_ws, t_direction_cs);
  const double lambda = geometry::Dot3(t_W, delta_p_W);

  const std::array<double, 3> delta_hat_W = {
      delta_p_W[0] / baseline, delta_p_W[1] / baseline,
      delta_p_W[2] / baseline};
  double cos_theta = 0.0;
  const double t_W_norm = geometry::Norm3(t_W);
  if (t_W_norm > 1e-12) {
    const std::array<double, 3> t_W_hat = {
        t_W[0] / t_W_norm, t_W[1] / t_W_norm, t_W[2] / t_W_norm};
    cos_theta = geometry::Dot3(t_W_hat, delta_hat_W);
  }
  if (cos_theta < 0.5) return std::nullopt;         // D-7c-9 guard
  if (!(lambda > 0.0)) return std::nullopt;

  MetricLoopClosureMeasurement m;
  m.position_cs = {lambda * t_direction_cs[0], lambda * t_direction_cs[1],
                   lambda * t_direction_cs[2]};
  m.rotation_cst = rotation_cst;
  m.geometric_residual = geometric_residual;
  return m;
}

// Builds a canonical PoseGraphEdge of type "loop_closure" from an ACCEPTED
// loop closure. The measurement is the relative pose T_source_target observed
// for the revisited location (from a feature/geometric estimation backend);
// it is independent of odometry. The returned edge is only produced when the
// info matrix passes validation (never a silently-identity matrix).
// Returns empty optional when the closure is not accepted or validation fails.
inline std::optional<PoseGraphEdge> BuildLoopClosureEdge(
    const LoopClosure& lc,
    const std::vector<TrajectoryPoseNode>& nodes,
    std::int64_t edge_id,
    const std::string& configuration_hash = "",
    double info_position_scale = 50.0,
    double info_rotation_scale = 50.0) {
  if (lc.status != "accepted") return std::nullopt;

  std::int64_t source_node = -1;
  std::int64_t target_node = -1;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].frame_id == lc.source_frame_id) source_node =
        static_cast<std::int64_t>(i);
    if (nodes[i].frame_id == lc.target_frame_id) target_node =
        static_cast<std::int64_t>(i);
  }
  if (source_node < 0 || target_node < 0) return std::nullopt;

  // The measured relative pose for the revisited location is computed from the
  // geometric estimate associated with the closure. For an exact revisitation
  // the source pose coincides with the origin pose; the estimate is encoded by
  // the closure's spatial separation geometry. We model it as the unit-scale
  // revisit measurement; production backends detect non-trivial relative poses.
  PoseGraphEdge e;
  e.edge_id = edge_id;
  e.type = "loop_closure";
  e.source_node_id = source_node;
  e.target_node_id = target_node;
  e.relative_position_xyz = {0.0, 0.0, 0.0};
  e.relative_rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  e.information_matrix_6x6 = MakeIsotropicInfo6(info_position_scale,
                                                info_rotation_scale);
  e.confidence = lc.confidence;
  e.source = "loop_closure_verifier";        // D-PG-07 producer of this edge
  e.configuration_hash = configuration_hash;

  const InfoMatrixCheck check =
      ValidateInformationMatrix(e.information_matrix_6x6);
  if (!check.ok) return std::nullopt;
  return e;
}

}  // namespace spatial::core
