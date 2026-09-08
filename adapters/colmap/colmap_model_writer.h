#pragma once

// Native COLMAP model WRITER (P3-impl-8c, P16; docs/architecture/
// P3-impl-8c-bundle-adjustment-readiness.md §2 P16/P5, §5). The strict inverse
// of ParseSparseModel / SparseModelToReconstruction: canonical
// Reconstruction (v3) -> cameras.bin / images.bin / points3D.bin, the missing
// prerequisite for feeding a canonical reconstruction to `colmap
// bundle_adjuster`.
//
// This TU is the sibling of colmap_converter.cpp and shares its exclusive
// native-format boundary rule: native COLMAP binary formats are written (and
// read) only inside adapters/colmap, reached by nothing else in the tree
// (reader rule, colmap_converter.h). Writers and readers must stay byte
// symmetric so a Write -> ParseSparseModel -> SparseModelToReconstruction
// round trip reproduces the canonical input poses and tracks exactly.
//
// Determinism (ADR-020): cameras/images/points3D are written sorted by id,
// point2D in index order, with no timestamps or ordering artifacts. Intrinsics
// are FIXED constants in 8c (D3): the native parameter vector reproduces the
// canonical fx/fy/cx/cy coefficients verbatim (P5).
//
// Native layout contract (single source of truth, colmap_converter.h:17-54):
//   cameras.bin:  u64 count; {u32 id, i32 model_id, u64 width, u64 height,
//                             double params[NumParams(model_id)]}
//   images.bin:   u64 count; {u32 id, double qvec[4] (w,x,y,z), double tvec[3],
//                             u32 camera_id, u32 name_len, char name[name_len],
//                             u64 num_points2D, per point {double x, double y,
//                             i64 point3D_id (-1 unobserved)}}
//   points3D.bin: u64 count; {u64 id, double xyz[3], u8 rgb[3], double error,
//                             u64 track_len, per track {u32 image_id,
//                             u32 point2D_idx}}
//
// Supported camera mappings (lossless, deterministic):
//   intrinsic_model "pinhole"         + distortion "none"           -> PINHOLE (id 1)
//   "opencv"         + "opencv_radial"                              -> OPENCV  (id 4)
//   "opencv_fisheye" + "opencv_fisheye"                             -> OPENCV_FISHEYE (id 5)
// Anything else (fov/custom/...) FAILS CLOSED with AdapterError: 8c never
// lets COLMAP silently reinterpret a camera model it cannot represent exactly.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/reconstruction/reconstruction.h"

namespace spatial::adapters::colmap {

// 2D keypoint coordinates (pixels) for one image, indexed by point2d_idx.
// Resolved by the caller from the frame_id -> FeatureSet -> FeatureArtifact
// chain (2D is never stored inline in the reconstruction document,
// reprojection.h); the writer needs the FULL list so images.bin carries the
// correct num_points2D and the x/y of every entry.
using ColmapKeypoints = std::vector<std::array<double, 2>>;

// Writes a canonical Reconstruction into `model_dir` as a complete native
// COLMAP model (cameras.bin / images.bin / points3D.bin), overwriting any
// existing files there.
//
// `keypoints_by_image_id` must provide, for EVERY ReconImage.camera... image,
// the keypoints whose index space the points3D tracks reference
// (point2d_idx). A missing image, an out-of-range or negative point2d_idx,
// non-finite geometry / keypoints, duplicate native ids, duplicate image
// names, negative image dimensions, or an unsupported camera model all FAIL
// CLOSED with spatial::core::AdapterError (ErrorCode::kAdapterProcessFailed)
// BEFORE any file is touched — a partial or corrupt native model is never
// emitted.
//
// Poses are converted canonically: ReconPose (T_reconstruction_camera,
// scalar-last quaternion) -> COLMAP (qvec_wxyz, tvec) as the exact algebraic
// inverse of InvertColmapPose (colmap_converter.cpp:477-494); the round trip
// reproduces the input pose. Image names are preserved verbatim from
// ReconImage.name (COLMAP's image identity inside a model).
void WriteColmapModel(
    const std::filesystem::path& model_dir,
    const spatial::core::Reconstruction& rec,
    const std::map<std::uint32_t, ColmapKeypoints>& keypoints_by_image_id);

}  // namespace spatial::adapters::colmap