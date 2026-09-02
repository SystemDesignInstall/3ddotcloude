#pragma once

// P3-impl-7 (D-RI-01..D-RI-04): optimized-trajectory -> Reconstruction v2
// pose-only feedback seam (Option A).
//
// This seam feeds a corrected (post-GTSAM) trajectory back into a NEW P2.5
// Reconstruction revision. Resolved decisions (docs/architecture/
// P3-impl-7-blocker-resolution.md):
//   CF-1 : caller supplies `reconstruction_from_trajectory` (SE3); the seam
//          never infers a frame relationship from `coordinate_frame` strings.
//          Same-frame callers declare Identity explicitly; when the caller
//          knows the frames differ but has NO alignment, it clears
//          `alignment_resolved` and the seam fails closed (typed error,
//          nothing written).
//   LC-1 : the source Reconstruction row is superseded via MetadataDb
//          (prerequisite, Phase 1). This seam produces the new document.
//   SC-1 : only `ReconImage.pose` of matched images is updated. Points,
//          cameras, and `detected` are preserved verbatim. No re-triangulation,
//          no bundle adjustment, no point-cloud transformation.
//
// The seam operates only on canonical P2.5/P3 types (D-AB-01/D-AB-02); no
// GTSAM or COLMAP dependency. It never mutates the source Reconstruction;
// it returns a NEW revision (immutability).

#include <array>
#include <string>
#include <vector>

#include "core/geometry/se3.h"
#include "core/reconstruction/reconstruction.h"
#include "core/trajectory/optimization.h"
#include "core/trajectory/trajectory.h"

namespace spatial::core {

// --- P3-impl-7 input (D-RI-01..D-RI-04, Mapping §12.1) ---

struct ReconstructionFeedbackInput {
  Reconstruction source;                    // original v1 (immutable read)
  Trajectory trajectory;                    // for coordinate_frame + provenance
  std::vector<TrajectoryPoseNode> trajectory_nodes;  // original nodes (cross-check)
  OptimizationResult optimization_result;   // result_id, status, provenance
  std::vector<OptimizedPoseNode> optimized_nodes;    // corrected T_trajectory_camera
  geometry::SE3 reconstruction_from_trajectory;      // T_reconstruction_trajectory (CF-1)
  bool alignment_resolved = true;           // false => frames differ, no alignment (fail closed)
};

// --- P3-impl-7 output ---

struct ReconstructionFeedbackResult {
  Reconstruction reconstruction;            // NEW revision (v2)
};

// Trivial diagnostic detail, one entry per input image (unchanged order).
struct PoseFeedbackDetail {
  std::string frame_id;
  bool updated = false;      // pose replaced from optimized node
  bool matched_node = false; // an OptimizedPoseNode existed for this frame
  bool preserved = false;    // kept original pose (no node / missing frame_id / not detected)
  bool skipped = false;      // not eligible (empty frame_id, or no node)
};

// Applies an optimized trajectory to a source reconstruction producing a NEW
// revision (Option A: camera poses only). Deterministic (Mapping §11); joins
// by frame_id string equality (Mapping §3.2); applies
//   T_reconstruction_camera = reconstruction_from_trajectory * T_trajectory_camera
// (Mapping §4.6, CF-1). Unmatched / missing frame_id are logged and skipped
// (Mapping §9-§10), never fatal.
//
// Throws (std::invalid_argument) when:
//   - the optimization result is empty (`optimized_nodes` has no nodes), or
//   - `alignment_resolved` is false (frames differ and no alignment is
//     available) -- fail closed (Mapping §5.4, test #5); nothing is written.
ReconstructionFeedbackResult ApplyOptimizedTrajectory(
    const ReconstructionFeedbackInput& input,
    std::vector<PoseFeedbackDetail>* details_out = nullptr);

// Deterministic self-consistency validation of a produced document:
// schema_version-2 shape, status="succeeded", non-empty reconstruction_id/
// scene_id, provenance backend populated, identical cam/point/image counts
// vs source, numeric sanity of updated poses. Returns true if ready for CAS.
bool ValidateOptimizedReconstruction(const Reconstruction& r);

// Builds an SE3 "world-from-body" pose from position + scalar-last quaternion.
geometry::SE3 MakePoseSe3(const std::array<double, 3>& position_xyz,
                          const std::array<double, 4>& rotation_xyzw);

// Writes an SE3 back into a ReconPose (position + scalar-last quaternion).
ReconPose MakeReconPose(const geometry::SE3& pose);

}  // namespace spatial::core
