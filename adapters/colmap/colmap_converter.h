#pragma once

// COLMAP-native -> canonical SparseModel converter (C1-S4; RFC-0008 §6/§7,
// RFC-0009 §5; plan §1.1, §6). The ONLY translation unit allowed to read
// COLMAP's native binary formats (cameras.bin / images.bin / points3D.bin).
//
// sparse-model.schema.json is deferred to P2.5; until then the converter
// emits a documented, provisional JSON document (schema_version: 1) that
// P2.5's schema supersedes. No schema file and no schema validation exist yet
// (colmap-p0-c1-s4-design.md D3).
//
// Determinism (ADR-020): equal native models always produce identical bytes.
// Cameras / images / points3D are emitted sorted by id, number formatting is
// canonical (nlohmann shortest round-trip), and the document carries no
// timestamps, uuids, or ordering artifacts.
//
// # COLMAP binary layout (single source of truth for the test fixture
// # builder; every integer/double is little-endian)
//
// cameras.bin:
//   uint64 num_cameras
//   per camera:
//     uint32 camera_id
//     int32  model_id          (table below)
//     uint64 width
//     uint64 height
//     double params[NumParams(model_id)]
//
// images.bin:
//   uint64 num_images
//   per image:
//     uint32 image_id
//     double qvec[4]           (w, x, y, z)
//     double tvec[3]
//     uint32 camera_id
//     uint32 name_len
//     char   name[name_len]
//     uint64 num_points2D
//     per point2D:
//       double x, y
//       int64  point3D_id      (-1 when unobserved)
//
// points3D.bin:
//   uint64 num_points3D
//   per point3D:
//     uint64 point3D_id
//     double xyz[3]
//     uint8  rgb[3]
//     double error
//     uint64 track_len
//     per track element:
//       uint32 image_id
//       uint32 point2D_idx
//
// # Camera model table (RFC-0009 §5; targets the calibration.schema.json
// # intrinsic_model vocabulary). `single_focal` models store one focal length
// # f that maps to both fx and fy.
//
//   id  model                    params  intrinsic_model    single_focal
//   0   SIMPLE_PINHOLE           3       pinhole            yes
//   1   PINHOLE                  4       pinhole            no
//   2   SIMPLE_RADIAL            4       opencv             yes   (subset)
//   3   RADIAL                   5       opencv             yes   (subset)
//   4   OPENCV                   8       opencv             no
//   5   OPENCV_FISHEYE           8       opencv_fisheye     no
//   6   FULL_OPENCV              12      custom             no    (not classified)
//   7   FOV                      5       fov                no
//   8   SIMPLE_RADIAL_FISHEYE    4       opencv_fisheye     yes   (subset)
//   9   RADIAL_FISHEYE           5       opencv_fisheye     yes   (subset)
//   10  THIN_PRISM_FISHEYE       12      custom             no
//
//   intrinsics extraction:
//     single_focal: fx = fy = params[0], cx = params[1], cy = params[2]
//     otherwise:    fx = params[0], fy = params[1], cx = params[2], cy = params[3]
//
// Distortion coefficients are intentionally not carried by the provisional
// v1 document (design §1.3 carries only `intrinsics {fx, fy, cx, cy}`); the
// native `model` name preserves what the source camera was. P2.5's schema is
// the canonical full-intrinsics carrier.

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/reconstruction/reconstruction.h"

namespace spatial::adapters::colmap {

// Pinhole intrinsics carried by the provisional document, in pixels.
struct SparseModelIntrinsics {
  double fx = 0.0;
  double fy = 0.0;
  double cx = 0.0;
  double cy = 0.0;

  bool operator==(const SparseModelIntrinsics&) const = default;
};

// One COLMAP camera. `model` is the native COLMAP model name (provenance);
// `intrinsic_model` is the RFC-0009 §5 mapping into the
// calibration.schema.json vocabulary.
struct SparseModelCamera {
  std::uint32_t camera_id = 0;
  std::string model;
  std::string intrinsic_model;
  SparseModelIntrinsics intrinsics;
  std::int64_t width = 0;
  std::int64_t height = 0;
  int model_id = 0;                        // COLMAP camera model id (for v2 distortion extraction)
  std::vector<double> raw_params;           // full parameter vector from cameras.bin (for v2)

  bool operator==(const SparseModelCamera&) const = default;
};

// One observed image. `qvec` is the camera-to-world quaternion in COLMAP's
// native (w, x, y, z) order; `tvec` is the camera-to-world translation. The
// JSON document carries them as `qvec_xyzw` and `tvec_xyz` respectively.
struct SparseModelImage {
  std::uint32_t image_id = 0;
  std::uint32_t camera_id = 0;
  std::string name;
  std::array<double, 4> qvec{};  // (w, x, y, z)
  std::array<double, 3> tvec{};

  bool operator==(const SparseModelImage&) const = default;
};

// One observation of a point3D from an image.
struct SparseModelTrackElement {
  std::uint32_t image_id = 0;
  std::uint32_t point2d_idx = 0;

  bool operator==(const SparseModelTrackElement&) const = default;
};

// One reconstructed 3D point with its observation track.
struct SparseModelPoint {
  std::uint64_t point3d_id = 0;
  std::array<double, 3> xyz{};
  std::array<std::uint8_t, 3> rgb{};
  double error = 0.0;
  std::vector<SparseModelTrackElement> track;

  bool operator==(const SparseModelPoint&) const = default;
};

// The provisional SparseModel document (design §1.3), post-parse. Cameras,
// images and points3D are kept sorted by id (deterministic serialization).
struct SparseModel {
  int schema_version = 1;  // provisional; superseded by P2.5
  std::vector<SparseModelCamera> cameras;
  std::vector<SparseModelImage> images;
  std::vector<SparseModelPoint> points3d;

  bool operator==(const SparseModel&) const = default;
};

// Parses the three COLMAP native files into the provisional document.
// Throws spatial::core::AdapterError (ErrorCode::kAdapterProcessFailed) on
// a missing/unreadable file, a truncated record, a count that runs past the
// end of the file, or an unknown camera model id. Never returns a partial
// document.
SparseModel ParseSparseModel(const std::filesystem::path& cameras_bin,
                             const std::filesystem::path& images_bin,
                             const std::filesystem::path& points3d_bin);

// Canonical serialization of the provisional document: deterministic field
// order and canonical number formatting. Equal SparseModel values always
// produce identical bytes (ADR-020).
std::string SparseModelToJson(const SparseModel& model);

// --- P2.5: Canonical Reconstruction v2 ---

// Provenance metadata for the v2 reconstruction document. The adapter
// populates this from its build info and execution context.
struct ReconstructionProvenanceInfo {
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

// Converts a parsed SparseModel (from ParseSparseModel) into the canonical
// Reconstruction v2 type (core/reconstruction/reconstruction.h).
//
// frame_id_map: optional mapping from COLMAP image name → Frame UUID string.
//   When provided, ReconImage.frame_id is populated. When empty, frame_id is
//   left as an empty string (downstream consumer resolves the mapping).
//
// The caller provides reconstruction-level metadata (IDs, coordinate frame,
// provenance) that the adapter obtains from its execution context.
//
// COLMAP-specific conversions:
//   - Quaternion: COLMAP (w,x,y,z) → canonical (x,y,z,w) [D-CRM-02]
//   - Pose: T_reconstruction_camera (COLMAP camera-to-world inverted) [D-CRM-01]
//   - Intrinsics + distortion: extracted per RFC-0009 camera model table
//   - All images included with detected=true (COLMAP only outputs registered)
spatial::core::Reconstruction SparseModelToReconstruction(
    const SparseModel& model,
    const std::string& reconstruction_id,
    const std::string& scene_id,
    const std::vector<std::string>& session_ids,
    const std::string& coordinate_frame,
    const ReconstructionProvenanceInfo& provenance,
    const std::map<std::string, std::string>& frame_id_map = {});

// Canonical JSON serialization of a Reconstruction v2 document.
// Deterministic field order and number formatting (ADR-020).
// The output conforms to schemas/json/reconstruction.schema.json.
std::string ReconstructionToJson(const spatial::core::Reconstruction& rec);

}  // namespace spatial::adapters::colmap
