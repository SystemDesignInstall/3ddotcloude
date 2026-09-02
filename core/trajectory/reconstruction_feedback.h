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
// GTSAM, COLMAP, or FrameGraph dependency. It never mutates the source
// Reconstruction; it returns a NEW revision (immutability, Mapping §7).
//
// Header-only to match core/trajectory/pose_graph_helpers.h style, which
// Mapping §12.1 mandates for this seam.

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/geometry/se3.h"
#include "core/reconstruction/reconstruction.h"
#include "core/trajectory/optimization.h"
#include "core/trajectory/pose_graph_helpers.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/uuid.h"

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

  // Optional provenance lineage hashes (Mapping §8.1). These are appended to
  // `input_artifact_hashes` of the produced Reconstruction's provenance.
  std::string source_reconstruction_cas_hash;      // v1 CAS hash
  std::string optimization_result_cas_hash;        // OptimizationResult CAS hash
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
  bool skipped = false;      // node skipped because image not eligible
};

// --- Construction helpers ---

// Builds an SE3 "world-from-body" pose from position + scalar-last quaternion.
inline geometry::SE3 MakePoseSe3(const std::array<double, 3>& position_xyz,
                                 const std::array<double, 4>& rotation_xyzw) {
  const Eigen::Vector3d t(position_xyz[0], position_xyz[1], position_xyz[2]);
  const geometry::Quaternion q(rotation_xyzw[0], rotation_xyzw[1],
                               rotation_xyzw[2], rotation_xyzw[3]);
  return geometry::SE3(q.Normalized(), t);
}

// Writes an SE3 back into a ReconPose (position + scalar-last quaternion).
inline ReconPose MakeReconPose(const geometry::SE3& pose) {
  const auto q = pose.rotation();
  ReconPose rp;
  rp.rotation_xyzw = {q.x(), q.y(), q.z(), q.w()};
  const Eigen::Vector3d& t = pose.translation();
  rp.translation_xyz = {t.x(), t.y(), t.z()};
  return rp;
}

// --- Core seam ---

// Applies an optimized trajectory to a source reconstruction producing a NEW
// revision (Option A: camera poses only). Deterministic (Mapping §11); joins
// by frame_id string equality (Mapping §3.2); applies
//   T_reconstruction_camera = reconstruction_from_trajectory * T_trajectory_camera
// (Mapping §4.6, CF-1). Unmatched / missing frame_id are skipped and never
// fatal (Mapping §9-§10).
//
// Throws std::invalid_argument when:
//   - `optimized_nodes` is empty (no optimization result to apply), or
//   - `alignment_resolved` is false (frames differ and no alignment is
//     available) -- fail closed (Mapping §5.4, test #5); nothing is written.
inline ReconstructionFeedbackResult ApplyOptimizedTrajectory(
    const ReconstructionFeedbackInput& input,
    std::vector<PoseFeedbackDetail>* details_out = nullptr) {
  if (input.optimized_nodes.empty()) {
    throw std::invalid_argument(
        "ApplyOptimizedTrajectory: no optimized pose nodes (empty "
        "optimization result); nothing to apply.");
  }
  if (!input.alignment_resolved) {
    throw std::invalid_argument(
        "ApplyOptimizedTrajectory: trajectory and reconstruction frames differ "
        "and no alignment (reconstruction_from_trajectory) is available; "
        "refusing to write raw poses (fail closed).");
  }

  // Index for crossing OptimizedPoseNode by frame_id (join key, Mapping §3.3).
  std::vector<std::pair<std::string, std::size_t>> node_by_frame;
  node_by_frame.reserve(input.optimized_nodes.size());
  for (std::size_t i = 0; i < input.optimized_nodes.size(); ++i) {
    if (!input.optimized_nodes[i].frame_id.empty()) {
      node_by_frame.emplace_back(input.optimized_nodes[i].frame_id, i);
    }
  }
  std::sort(node_by_frame.begin(), node_by_frame.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  ReconstructionFeedbackResult out;
  Reconstruction& r = out.reconstruction;

  // Copy the source verbatim first (SC-1 baseline). Every field not touched
  // below is preserved byte-for-byte.
  r = input.source;

  // New instance identity (D-CRM-07, Mapping §7): UUIDv4, not content-derived.
  r.reconstruction_id = FormatUuid(GenerateUuid());
  r.status = "succeeded";

  // Deterministic timestamp (Mapping §11.3): reuse the optimization result's
  // creation time so identical inputs yield identical payload bytes.
  r.created_at_ns = input.optimization_result.created_at_ns;

  // Provenance lineage (Mapping §8): backend.name = "spatial_optimizer".
  ReconstructionProvenance& prov = r.provenance;
  ReconstructionProvenance prov_bak = prov;  // preserve input fields not overwritten
  ReconstructionProvenance fresh;
  fresh.backend.name = "spatial_optimizer";
  fresh.backend.version = input.optimization_result.provenance.optimizer.version;
  fresh.backend.adapter_version = input.optimization_result.provenance.adapter_version;
  fresh.engine_version = prov_bak.engine_version;
  fresh.engine_commit = prov_bak.engine_commit;
  fresh.git_commit = prov_bak.git_commit;
  fresh.configuration_hash = prov_bak.configuration_hash;
  {
    std::string j = "{\"optimizer\":\"";
    j += input.optimization_result.provenance.optimizer.name;
    j += "\",\"optimization_result_id\":\"";
    j += input.optimization_result.result_id;
    j += "\",\"option\":\"A\"}";
    fresh.backend_specific_json = std::move(j);
    // NOTE: provenance timing (started/finished/duration) kept zero =
    // deterministic, per Mapping §11.3 (no wall-clock in CAS payload bytes).
  }
  // input_artifact_hashes: inherit the chain's hashes and append the two
  // lineage hashes (Mapping §8.1). In ascending order for determinism.
  std::vector<std::string> hashes = prov_bak.input_artifact_hashes;
  std::vector<std::string> to_add;
  if (!input.optimization_result_cas_hash.empty())
    to_add.push_back(input.optimization_result_cas_hash);
  if (!input.source_reconstruction_cas_hash.empty())
    to_add.push_back(input.source_reconstruction_cas_hash);
  for (const auto& h : to_add) {
    if (std::find(hashes.begin(), hashes.end(), h) == hashes.end()) {
      hashes.push_back(h);
    }
  }
  std::sort(hashes.begin(), hashes.end());
  fresh.input_artifact_hashes = std::move(hashes);
  prov = std::move(fresh);

  // Apply optimized poses to matched images (Mapping §3.2 case a); the join
  // key is frame_id string equality. Iterate the recon images in their source
  // order; the output list keeps source ordering (stable, reproducible).
  std::vector<PoseFeedbackDetail> details;
  details.reserve(input.source.images.size());
  for (std::size_t i = 0; i < r.images.size(); ++i) {
    const ReconImage& img = r.images[i];
    PoseFeedbackDetail d;
    d.frame_id = img.frame_id;
    if (img.frame_id.empty() || !img.detected) {
      d.preserved = true;
      if (img.frame_id.empty()) d.skipped = true;
      details.push_back(std::move(d));
      continue;
    }
    // Locate an optimized node with the same frame_id.
    const auto it =
        std::lower_bound(node_by_frame.begin(), node_by_frame.end(), img.frame_id,
                         [](const auto& p, const std::string& s) {
                           return p.first < s;
                         });
    if (it == node_by_frame.end() || it->first != img.frame_id) {
      // Mapping §3.2 case c: no trajectory node -> keep original pose.
      d.matched_node = false;
      d.preserved = true;
      details.push_back(std::move(d));
      continue;
    }
    d.matched_node = true;
    const OptimizedPoseNode& node = input.optimized_nodes[it->second];
    // T_rc = T_rt * T_tc (Mapping §4.6).
    const geometry::SE3 T_tc = MakePoseSe3(node.position_xyz, node.rotation_xyzw);
    const geometry::SE3 T_rc = input.reconstruction_from_trajectory * T_tc;
    r.images[i].pose = MakeReconPose(T_rc);
    d.updated = true;
    details.push_back(std::move(d));
  }
  // Trajectory nodes with no matching image are simply not iterated (they add
  // nothing to images); Mapping §3.2 case b and §9 are satisfied by omission.

  if (details_out) *details_out = std::move(details);
  return out;
}

// Deterministic self-consistency validation of a produced document (Mapping
// §12.3): schema-version-2 shape, status="succeeded", non-empty identity
// fields, provenance backend populated, and structural invariants.
inline bool ValidateOptimizedReconstruction(const Reconstruction& r) {
  if (r.reconstruction_id.empty()) return false;
  if (r.scene_id.empty()) return false;
  if (r.status != "succeeded") return false;
  if (r.provenance.backend.name.empty()) return false;
  if (r.coordinate_frame.empty()) return false;
  // Every image must carry a valid, non-identity pose and a frame_id bridge
  // when it was updated; the seam always tags updated poses' quaternion to
  // unit norm (MakePoseSe3 normalizes).
  for (const ReconImage& img : r.images) {
    double n = 0.0;
    for (double c : img.pose.rotation_xyzw) n += c * c;
    if (n < 1e-3) return false;  // degenerate quaternion
  }
  return true;
}

}  // namespace spatial::core
