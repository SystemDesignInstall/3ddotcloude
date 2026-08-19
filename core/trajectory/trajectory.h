#pragma once

// Canonical Trajectory domain types (P3).
// Backend-independent representation of camera motion through time.
// Every trajectory backend (ORB-SLAM, KISS-ICP, COLMAP, future AI systems)
// produces these types through its adapter. The types carry no
// backend-specific fields; backend metadata lives in provenance.
//
// Normative decisions: D-TRJ-01 through D-TRJ-10 (P3-trajectory-pose-graph-loop-closure.md).
// Pose convention: T_trajectory_camera (NOT T_world_camera unless aligned).
// Quaternion order: (x, y, z, w) scalar-last per ADR-007.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/reconstruction/reconstruction.h"

namespace spatial::core {

// --- Trajectory Uncertainty (D-TRJ-06) ---

struct TrajectoryUncertainty {
  double mean_position_uncertainty_m = 0.0;
  double max_position_uncertainty_m = 0.0;
  double mean_rotation_uncertainty_rad = 0.0;
  double max_rotation_uncertainty_rad = 0.0;
  double loop_closure_density = 0.0;  // closures per metre of trajectory
  bool operator==(const TrajectoryUncertainty&) const = default;
};

// --- Pose Payload Node (D-TRJ-07, D-TRJ-08, D-TRJ-09) ---
// Per-pose data in a CAS payload referenced by the trajectory.
// Position and rotation are T_trajectory_camera (world_from_body
// in the trajectory's local frame).

struct TrajectoryPoseNode {
  std::string frame_id;                    // UUID string -> Frame (bridge, D-TRJ-10)
  std::int64_t timestamp_ns = 0;
  std::int64_t sequence_index = 0;         // monotonically increasing, gaps allowed
  std::array<double, 3> position_xyz{};    // translation of T_trajectory_camera
  std::array<double, 4> rotation_xyzw{};   // (x, y, z, w) scalar-last
  std::array<double, 6> covariance_position{};  // 3x3 upper triangle (xx,xy,xz,yy,yz,zz)
  std::array<double, 6> covariance_rotation{};  // 3x3 upper triangle
  bool operator==(const TrajectoryPoseNode&) const = default;
};

// --- Trajectory (root entity, D-TRJ-01) ---

struct Trajectory {
  std::string trajectory_id;               // UUIDv4, instance-scoped (D-TRJ-02)
  std::string scene_id;                    // owning scene
  std::string session_id;                  // capture session this trajectory belongs to
  std::string kind;                        // "odometry" | "slam" | "sfm" | "survey" (D-TRJ-03)
  std::string coordinate_frame;            // e.g. "trajectory_0" (D-TRJ-04)
  std::string status;                      // "building" | "optimized" | "superseded" (D-TRJ-05)
  std::int64_t created_at_ns = 0;
  std::int64_t node_count = 0;             // number of pose nodes (quick display)
  double total_distance_m = 0.0;           // path length in metres (quick display)
  double total_duration_ns = 0.0;          // time span in nanoseconds
  TrajectoryUncertainty uncertainty;       // aggregate uncertainty metrics (D-TRJ-06)
  ReconstructionProvenance provenance;     // reuse P2.5 provenance type
  bool operator==(const Trajectory&) const = default;
};

}  // namespace spatial::core
