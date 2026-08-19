#include "adapters/colmap/colmap_trajectory_adapter.h"

#include <cmath>
#include <numeric>
#include <stdexcept>

#include "core/errors/project_error.h"

namespace spatial::adapters::colmap {
namespace {

// ---------------------------------------------------------------------------
// Quaternion math — identical to colmap_converter.cpp anonymous namespace.
// These are pure mathematical operations (reorder + conjugate + rotate)
// with no COLMAP-specific semantics; they implement the InvertColmapPose
// transform documented in colmap_converter.cpp:475-494.
// ---------------------------------------------------------------------------

// Conjugate of a unit quaternion: q* = (-x, -y, -z, w).
void ConjugateQuaternion(const std::array<double, 4>& q,
                         std::array<double, 4>& out) {
  out[0] = -q[0];
  out[1] = -q[1];
  out[2] = -q[2];
  out[3] =  q[3];
}

// Rotate a 3D vector by a unit quaternion: v' = q * v * q*.
void RotateVectorByQuaternion(const std::array<double, 4>& q,
                              const std::array<double, 3>& v,
                              std::array<double, 3>& out) {
  // q = (x, y, z, w)
  double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
  double vx = v[0], vy = v[1], vz = v[2];

  // t = 2 * cross(q_xyz, v)
  double tx = 2.0 * (qy*vz - qz*vy);
  double ty = 2.0 * (qz*vx - qx*vz);
  double tz = 2.0 * (qx*vy - qy*vx);

  // v' = v + w*t + cross(q_xyz, t)
  out[0] = vx + qw*tx + (qy*tz - qz*ty);
  out[1] = vy + qw*ty + (qz*tx - qx*tz);
  out[2] = vz + qw*tz + (qx*ty - qy*tx);
}

// Convert COLMAP camera-to-world pose to T_trajectory_camera
// (world-from-camera): R_cw = R_wc^T, t_cw = -R_wc^T * t_wc.
//
// COLMAP qvec is (w, x, y, z). We reorder to (x, y, z, w) per ADR-007,
// then conjugate (= invert rotation), then compute the inverted translation.
//
// This is the SAME mathematical path as InvertColmapPose() in
// colmap_converter.cpp:477-494, which has been validated by the P2.5
// reconstruction adapter tests. The semantic mapping is:
//   COLMAP camera-to-world → T_trajectory_camera (D-TRJ-08)
// For COLMAP's reconstruction frame, T_trajectory_camera = Invert(T_camworld).
void InvertColmapPose(const std::array<double, 4>& qvec_wxyz,
                      const std::array<double, 3>& tvec,
                      std::array<double, 4>& rot_xyzw,
                      std::array<double, 3>& trans) {
  // COLMAP qvec is (w, x, y, z). Convert to our (x, y, z, w).
  std::array<double, 4> q_xyzw = {qvec_wxyz[1], qvec_wxyz[2],
                                   qvec_wxyz[3], qvec_wxyz[0]};
  // Conjugate = inverse rotation.
  std::array<double, 4> q_inv;
  ConjugateQuaternion(q_xyzw, q_inv);
  // t_cw = -R_wc^T * t_wc = -R_wc^{-1} * t_wc = -(q_inv * t_wc * q_inv*).
  std::array<double, 3> neg_t = {-tvec[0], -tvec[1], -tvec[2]};
  std::array<double, 3> rotated;
  RotateVectorByQuaternion(q_inv, neg_t, rotated);
  // Output.
  rot_xyzw = q_inv;  // already (x, y, z, w)
  trans = rotated;
}

}  // namespace

core::TrajectoryExtractionResult SparseModelToTrajectory(
    const SparseModel& model,
    const std::string& trajectory_id,
    const std::string& scene_id,
    const std::string& session_id,
    const std::string& coordinate_frame,
    const TrajectoryProvenanceInfo& prov_info,
    const std::map<std::string, std::string>& frame_id_map) {

  if (model.images.empty()) {
    throw spatial::core::AdapterError(
        spatial::core::ErrorCode::kAdapterProcessFailed,
        "Cannot extract trajectory from empty COLMAP model (no images)",
        {},
        /*recoverable=*/false,
        "Run COLMAP sparse reconstruction first to produce registered images.");
  }

  core::TrajectoryExtractionResult result;

  // --- Trajectory metadata ---
  core::Trajectory& traj = result.trajectory;
  traj.trajectory_id = trajectory_id;
  traj.scene_id = scene_id;
  traj.session_id = session_id;
  traj.kind = "sfm";  // COLMAP is Structure-from-Motion (D-TRJ-03)
  traj.coordinate_frame = coordinate_frame;
  traj.status = "building";  // initial status (D-TRJ-05)

  // Provenance.
  traj.provenance.backend.name = prov_info.backend_name;
  traj.provenance.backend.version = prov_info.backend_version;
  traj.provenance.backend.adapter_version = prov_info.adapter_version;
  traj.provenance.configuration_hash = prov_info.configuration_hash;
  traj.provenance.input_artifact_hashes = prov_info.input_artifact_hashes;
  traj.provenance.engine_version = prov_info.engine_version;
  traj.provenance.engine_commit = prov_info.engine_commit;
  traj.provenance.git_commit = prov_info.git_commit;
  traj.provenance.started_at_ns = prov_info.started_at_ns;
  traj.provenance.finished_at_ns = prov_info.finished_at_ns;
  traj.provenance.duration_ns = prov_info.duration_ns;

  // --- Pose nodes ---
  // COLMAP images are sorted by image_id in the parsed SparseModel
  // (deterministic order, D-CRM-20). We assign sequence_index 0..N-1.
  result.nodes.reserve(model.images.size());
  std::int64_t seq = 0;
  double total_distance = 0.0;
  std::array<double, 3> prev_position{0.0, 0.0, 0.0};
  bool has_prev = false;

  for (const SparseModelImage& img : model.images) {
    core::TrajectoryPoseNode node;

    // Frame ID: resolve from optional frame_id_map (D-TRJ-10).
    auto it = frame_id_map.find(img.name);
    if (it != frame_id_map.end()) {
      node.frame_id = it->second;
    }
    // If not found, frame_id remains empty (caller resolves later).

    // Timestamp: COLMAP images.bin does not carry timestamps.
    // Must NOT be fabricated. The downstream consumer resolves timestamps
    // from the Frame/ImportModel layer.
    node.timestamp_ns = 0;

    // Sequence index: monotonic, 0..N-1 in image_id sort order.
    node.sequence_index = seq++;

    // Pose: COLMAP camera-to-world → T_trajectory_camera (D-TRJ-08).
    // Same mathematical path as InvertColmapPose() in colmap_converter.cpp.
    // Validated by non-trivial rotation tests in test_trajectory_adapter.cpp.
    InvertColmapPose(img.qvec, img.tvec,
                     node.rotation_xyzw, node.position_xyz);

    // Accumulate total distance (Euclidean between consecutive poses).
    if (has_prev) {
      double dx = node.position_xyz[0] - prev_position[0];
      double dy = node.position_xyz[1] - prev_position[1];
      double dz = node.position_xyz[2] - prev_position[2];
      total_distance += std::sqrt(dx*dx + dy*dy + dz*dz);
    }
    prev_position = node.position_xyz;
    has_prev = true;

    result.nodes.push_back(std::move(node));
  }

  traj.node_count = static_cast<std::int64_t>(result.nodes.size());
  traj.total_distance_m = total_distance;

  // total_duration_ns remains 0 — COLMAP has no timestamps to compute it.
  traj.total_duration_ns = 0.0;

  return result;
}

}  // namespace spatial::adapters::colmap
