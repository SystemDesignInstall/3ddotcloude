#pragma once

// Trajectory Adapter Interface (P3-impl-3).
// Backend-independent result type for trajectory extraction.
// Every trajectory backend (COLMAP, ORB-SLAM, KISS-ICP, etc.) produces
// these types through its adapter. The interface carries no backend-specific
// fields; backend metadata lives in Trajectory.provenance.
//
// This header defines ONLY the result container and the adapter
// function concept. Backend-specific adapter implementations live in
// adapters/<backend>/ (architecture boundary: core/trajectory must not
// know about any backend).
//
// Pose convention: T_trajectory_camera (D-TRJ-08).
// Quaternion order: (x, y, z, w) scalar-last per ADR-007.
// Timestamp: when the backend cannot provide timestamps (e.g. COLMAP),
// timestamp_ns is 0 and must NOT be fabricated. The downstream consumer
// resolves timestamps from the Frame/ImportModel layer.
// Sequence: when the backend provides images in file order but no explicit
// ordering, sequence_index is assigned 0..N-1 in file order.

#include <cstdint>
#include <string>
#include <vector>

#include "core/trajectory/trajectory.h"

namespace spatial::core {

// Result of a trajectory extraction adapter call.
// Contains the trajectory metadata and its ordered pose nodes.
// The adapter populates trajectory_id, scene_id, session_id,
// coordinate_frame, kind, status, provenance, and the node vector.
// node_count and total_distance_m are computed by the adapter.
struct TrajectoryExtractionResult {
  Trajectory trajectory;
  std::vector<TrajectoryPoseNode> nodes;
};

}  // namespace spatial::core
