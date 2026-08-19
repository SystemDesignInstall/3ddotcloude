#pragma once

// COLMAP Trajectory Extraction Adapter (P3-impl-3).
// Converts a parsed COLMAP SparseModel into the canonical P3 Trajectory
// representation (Trajectory + ordered TrajectoryPoseNode vector).
//
// COLMAP-specific knowledge isolated here:
//   - Quaternion reorder: COLMAP (w,x,y,z) → canonical (x,y,z,w) [ADR-007]
//   - Pose direction: COLMAP camera-to-world → T_trajectory_camera
//     (world-from-camera) [D-TRJ-08]
//   - Timestamp: COLMAP images.bin has no timestamps → timestamp_ns = 0
//     (downstream resolves from Frame/ImportModel, never fabricated)
//   - Sequence: COLMAP images sorted by image_id → sequence_index 0..N-1
//   - Frame ID: optional frame_id_map (image name → Frame UUID) [D-TRJ-10]
//
// The pose conversion follows the same mathematical path as the P2.5
// SparseModelToReconstruction InvertColmapPose(): given COLMAP's
// (qvec_wxyz, tvec) representing T_camworld (camera-to-world), we compute
// T_trajectory_camera = Invert(T_camworld) for COLMAP's reconstruction frame.
// This is validated by non-trivial rotation tests in test_trajectory_adapter.cpp.
//
// Provenance: the caller populates Trajectory.provenance from its execution
// context (backend name/version, adapter version, configuration hash, etc.).

#include <map>
#include <string>

#include "adapters/colmap/colmap_converter.h"
#include "core/trajectory/trajectory_adapter.h"

namespace spatial::adapters::colmap {

// Provenance metadata for the trajectory extraction. The adapter
// populates Trajectory.provenance from this.
struct TrajectoryProvenanceInfo {
  std::string backend_name = "colmap";
  std::string backend_version;
  std::string adapter_version;
  std::string configuration_hash;
  std::vector<std::string> input_artifact_hashes;
  std::string engine_version;
  std::string engine_commit;
  std::string git_commit;
  std::int64_t started_at_ns = 0;
  std::int64_t finished_at_ns = 0;
  std::int64_t duration_ns = 0;
};

// Converts a parsed COLMAP SparseModel into the canonical P3 Trajectory
// representation.
//
// Parameters:
//   model            - parsed COLMAP sparse model (cameras + images + points3D)
//   trajectory_id    - UUIDv4 for the trajectory (D-TRJ-02)
//   scene_id         - owning scene UUID
//   session_id       - capture session UUID
//   coordinate_frame - trajectory coordinate frame label (D-TRJ-04)
//   provenance       - backend/adapter provenance metadata
//   frame_id_map     - optional mapping: COLMAP image name → Frame UUID (D-TRJ-10)
//
// Returns TrajectoryExtractionResult with:
//   - trajectory.metadata populated (IDs, kind="sfm", status="building")
//   - trajectory.node_count and total_distance_m computed
//   - nodes vector ordered by sequence_index (= image_id sort order)
//   - Each node: T_trajectory_camera from InvertColmapPose(), quaternion in
//     (x,y,z,w), timestamp_ns=0 (COLMAP has no timestamps)
//
// The caller must NOT fabricate timestamps — they are 0 and the downstream
// consumer resolves them from the Frame/ImportModel layer.
//
// Throws spatial::core::AdapterError if the model is empty.
core::TrajectoryExtractionResult SparseModelToTrajectory(
    const SparseModel& model,
    const std::string& trajectory_id,
    const std::string& scene_id,
    const std::string& session_id,
    const std::string& coordinate_frame,
    const TrajectoryProvenanceInfo& provenance,
    const std::map<std::string, std::string>& frame_id_map = {});

}  // namespace spatial::adapters::colmap
