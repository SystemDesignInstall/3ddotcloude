#pragma once

// Canonical Pose Graph domain types (P3).
// Backend-independent representation of spatial constraints between camera poses.
// Every pose graph backend (GTSAM, g2o, custom) produces/consumes these types
// through its adapter. The types carry no backend-specific fields.
//
// Normative decisions: D-PG-01 through D-PG-07 (P3-trajectory-pose-graph-loop-closure.md).
// Pose convention: relative transforms use T_source_target notation.
// Quaternion order: (x, y, z, w) scalar-last per ADR-007.
// Information matrix: flattened row-major, translation-then-rotation (ADR-007).

#include <cstdint>
#include <string>
#include <vector>

#include "core/reconstruction/reconstruction.h"

namespace spatial::core {

// --- Pose Graph Node (CAS payload, D-PG-03) ---
// Graph nodes carry only id + frame_id + timestamp. Pose data is NOT
// duplicated here — it lives in the Trajectory payload.

struct PoseGraphNode {
  std::int64_t node_id = 0;                // integer, ordered by sequence_index
  std::string frame_id;                    // UUID string -> Frame (bridge)
  std::int64_t timestamp_ns = 0;
  bool operator==(const PoseGraphNode&) const = default;
};

// --- Pose Graph Edge / Constraint (CAS payload, D-PG-04..D-PG-07) ---

struct PoseGraphEdge {
  std::int64_t edge_id = 0;                // assigned during graph assembly
  std::string type;                        // "odometry" | "loop_closure" | "prior" | "gps" |
                                           // "imu_preintegration" | "lidar_odometry" (D-PG-04)
  std::int64_t source_node_id = 0;         // -> PoseGraphNode.node_id
  std::int64_t target_node_id = 0;         // -> PoseGraphNode.node_id
  std::array<double, 3> relative_position_xyz{};   // T_source_target translation
  std::array<double, 4> relative_rotation_xyzw{};  // T_source_target rotation (x,y,z,w)
  std::array<double, 36> information_matrix_6x6{}; // flattened row-major (D-PG-05)
  double confidence = 0.0;                 // [0, 1] informational quality (D-PG-06)
  std::string source;                      // which algorithm produced this edge (D-PG-07)
  std::string configuration_hash;          // SHA-256 of effective configuration (D-PG-07)
  bool operator==(const PoseGraphEdge&) const = default;
};

// --- Pose Graph Constraint (DB-level edge representation) ---
// Simplified edge representation for DB persistence and querying.
// Full edge data lives in the CAS payload.

struct PoseGraphConstraint {
  std::int64_t source_node_id = 0;
  std::int64_t target_node_id = 0;
  std::string type;                        // same vocabulary as PoseGraphEdge.type
  double confidence = 0.0;
  bool operator==(const PoseGraphConstraint&) const = default;
};

// --- Pose Graph (root entity, D-PG-01) ---

struct PoseGraph {
  std::string graph_id;                    // UUIDv4
  std::string trajectory_id;               // the trajectory this graph was built from
  std::string scene_id;
  std::string status;                      // "building" | "ready" | "optimizing" | "optimized" | "failed" (D-PG-02)
  std::int64_t node_count = 0;
  std::int64_t edge_count = 0;
  std::int64_t odometry_edge_count = 0;
  std::int64_t loop_closure_edge_count = 0;
  std::int64_t prior_edge_count = 0;
  std::int64_t created_at_ns = 0;
  ReconstructionProvenance provenance;
  bool operator==(const PoseGraph&) const = default;
};

}  // namespace spatial::core
