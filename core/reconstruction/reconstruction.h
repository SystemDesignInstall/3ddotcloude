#pragma once

// Canonical Reconstruction Model types (P2.5).
// Backend-independent representation of a sparse reconstruction result.
// Every reconstruction backend (COLMAP, SLAM, hybrid, AI priors) produces
// these types through its adapter/converter. The types carry no
// backend-specific fields; backend metadata lives in provenance.backend_specific.
//
// Normative decisions: D-CRM-01 through D-CRM-20 (P2.5-canonical-reconstruction-model.md).
// Pose convention: T_reconstruction_camera (NOT T_world_camera unless aligned).
// Quaternion order: (x, y, z, w) scalar-last per ADR-007.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/utils/uuid.h"

namespace spatial::core {

// --- Pose (D-CRM-01, D-CRM-02, D-CRM-13) ---

struct ReconPose {
  std::array<double, 4> rotation_xyzw{};   // (x, y, z, w) scalar-last
  std::array<double, 3> translation_xyz{};
  bool operator==(const ReconPose&) const = default;
};

// --- Camera (D-CRM-04, D-CRM-05) ---

struct ReconCamera {
  std::uint32_t camera_id = 0;             // backend-local, reconstruction-scoped
  std::int64_t width = 0;
  std::int64_t height = 0;
  std::string intrinsic_model;              // "pinhole", "opencv", "opencv_fisheye", "fov", "omnidirectional", "custom"
  double fx = 0.0;
  double fy = 0.0;
  double cx = 0.0;
  double cy = 0.0;
  std::string distortion_model;             // "none", "opencv_radial", "opencv_fisheye", "equidistant", "custom"
  std::vector<double> distortion_coefficients;
  std::string calibration_ref;              // UUID string → CalibrationArtifact (optional, D-CRM-05)
  bool operator==(const ReconCamera&) const = default;
};

// --- Image (D-CRM-20) ---

struct ReconImage {
  std::uint32_t image_id = 0;              // backend-local, reconstruction-scoped
  std::uint32_t camera_id = 0;             // → ReconCamera.camera_id
  std::string frame_id;                    // UUID string → Frame (bridge to import model)
  std::string name;                        // original filename (provenance, not identity)
  ReconPose pose;                          // T_reconstruction_camera
  bool detected = false;                   // whether this image was successfully registered
  bool operator==(const ReconImage&) const = default;
};

// --- 3D Point (D-CRM-08) ---

struct ReconPoint3D {
  std::uint64_t point3d_id = 0;            // backend-local, reconstruction-scoped
  std::array<double, 3> xyz{};             // position in reconstruction frame (meters)
  std::array<std::uint8_t, 3> color{};     // RGB
  double error = 0.0;                      // mean reprojection error (pixels)
  struct TrackElement {
    std::uint32_t image_id = 0;            // → ReconImage.image_id
    std::int32_t point2d_idx = 0;          // index into FeatureArtifact.keypoints[]
    bool operator==(const TrackElement&) const = default;
  };
  std::vector<TrackElement> track;         // observations of this point
  bool operator==(const ReconPoint3D&) const = default;
};

// --- Observation (D-CRM-09, D-CRM-18) ---
// Atomic 2D→3D association. Carries only the link; 2D coordinates live in
// the FeatureArtifact referenced via the frame_id → FeatureSet chain.

struct ReconObservation {
  std::uint32_t image_id = 0;              // → ReconImage.image_id
  std::int32_t point2d_idx = 0;            // index into FeatureArtifact.keypoints[]
  bool operator==(const ReconObservation&) const = default;
};

// --- Provenance (D-CRM-11) ---

struct ReconstructionProvenance {
  struct BackendInfo {
    std::string name;                      // "colmap", "orbslam", "hybrid", etc.
    std::string version;                   // backend version string
    std::string adapter_version;           // our adapter version
    bool operator==(const BackendInfo&) const = default;
  };
  BackendInfo backend;
  std::string configuration_hash;          // SHA-256 of effective configuration
  std::vector<std::string> input_artifact_hashes;  // CAS hashes of input artifacts
  std::string engine_version;
  std::string engine_commit;
  std::string git_commit;                  // adapter git commit
  std::int64_t started_at_ns = 0;
  std::int64_t finished_at_ns = 0;
  std::int64_t duration_ns = 0;
  std::string backend_specific_json;       // arbitrary backend metadata (D-CRM-11)
  bool operator==(const ReconstructionProvenance&) const = default;
};

// --- Uncertainty (D-CRM-08) ---

struct ReconUncertainty {
  std::array<double, 6> covariance_xyz{};  // 3x3 symmetric, flattened row-major
  double confidence = 0.0;                 // [0, 1] posterior confidence
  std::int64_t source_count = 0;           // number of images observing this point
  bool operator==(const ReconUncertainty&) const = default;
};

// --- Reconstruction (root entity) ---

struct Reconstruction {
  std::string reconstruction_id;           // UUID string (D-CRM-07: instance-scoped, UUIDv4)
  std::string scene_id;                    // UUID string
  std::vector<std::string> session_ids;    // UUID strings (D-CRM-12)
  std::string coordinate_frame;            // e.g. "reconstruction_0" (D-CRM-06)
  std::string status;                      // "reconstructing" | "succeeded" | "failed" | "superseded" (D-CRM-19)
  std::int64_t created_at_ns = 0;
  ReconstructionProvenance provenance;
  std::vector<ReconCamera> cameras;
  std::vector<ReconImage> images;          // ALL images, including detected=false (D-CRM-20)
  std::vector<ReconPoint3D> points3D;
  bool operator==(const Reconstruction&) const = default;
};

}  // namespace spatial::core
