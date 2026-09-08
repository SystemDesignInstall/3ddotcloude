#include "adapters/colmap/colmap_model_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

#include "core/errors/project_error.h"

namespace spatial::adapters::colmap {

// --- Camera model id mapping (mirror of colmap_converter.h:59-70) ---

namespace {

using spatial::core::AdapterError;
using spatial::core::ErrorCode;
using spatial::core::ReconCamera;
using spatial::core::ReconImage;
using spatial::core::ReconPose;
using spatial::core::ReconPoint3D;
using spatial::core::Reconstruction;

constexpr int kPinHoleModel = 1;        // PINHOLE (non-single-focal): fx, fy, cx, cy
constexpr int kOpenCvModel = 4;         // OPENCV: fx, fy, cx, cy, k1, k2, p1, p2
constexpr int kOpenCvFishEyeModel = 5;  // OPENCV_FISHEYE: fx, fy, cx, cy, k1..k4

AdapterError WriterError(const std::string& message) {
  return AdapterError(
      ErrorCode::kAdapterProcessFailed, message, {},
      /*recoverable=*/false,
      "The canonical reconstruction cannot be represented as a native COLMAP "
      "model (P3-impl-8c P5/P12); 8c refuses to silently reinterpret it.");
}

// Deterministic little-endian binary writer (mirror of the shim's BinWriter).
class ColmapBinWriter {
 public:
  explicit ColmapBinWriter(const std::filesystem::path& path)
      : out_(path, std::ios::binary) {}
  bool ok() const { return static_cast<bool>(out_); }

  void U8(std::uint8_t v) { out_.put(static_cast<char>(v)); }
  void U32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out_.put(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void I32(std::int32_t v) { U32(static_cast<std::uint32_t>(v)); }
  void U64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out_.put(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void I64(std::int64_t v) { U64(static_cast<std::uint64_t>(v)); }
  void F64(double v) {
    if (!std::isfinite(v)) {
      throw WriterError("non-finite double cannot be written to a native "
                        "COLMAP model");
    }
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "IEEE-754 double expected");
    std::memcpy(&bits, &v, sizeof(bits));
    U64(bits);
  }
  void String(const std::string& v) {
    U32(static_cast<std::uint32_t>(v.size()));
    out_.write(v.data(), static_cast<std::streamsize>(v.size()));
  }

  void Close() { out_.close(); }

 private:
  std::ofstream out_;
};

// Conjugate of a unit quaternion (scalar-last): q* = (-x, -y, -z, w).
std::array<double, 4> ConjugateQuaternion(const std::array<double, 4>& q) {
  return {-q[0], -q[1], -q[2], q[3]};
}

// Rotate a 3D vector by a unit quaternion: v' = q * v * q*.
std::array<double, 3> RotateVectorByQuaternion(
    const std::array<double, 4>& q, const std::array<double, 3>& v) {
  const double qx = q[0], qy = q[1], qz = q[2], qw = q[3];
  const double vx = v[0], vy = v[1], vz = v[2];
  const double tx = 2.0 * (qy * vz - qz * vy);
  const double ty = 2.0 * (qz * vx - qx * vz);
  const double tz = 2.0 * (qx * vy - qy * vx);
  return {vx + qw * tx + (qy * tz - qz * ty),
          vy + qw * ty + (qz * tx - qx * tz),
          vz + qw * tz + (qx * ty - qy * tx)};
}

// ReconPose (T_reconstruction_camera, scalar-last) -> COLMAP (qvec_wxyz, tvec).
// Exact algebraic inverse of InvertColmapPose (colmap_converter.cpp:477-494).
void CanonicalPoseToColmap(const ReconPose& pose, double qvec_wxyz[4],
                           double tvec[3]) {
  const std::array<double, 4> q_conv = ConjugateQuaternion(pose.rotation_xyzw);
  qvec_wxyz[0] = q_conv[3];  // w
  qvec_wxyz[1] = q_conv[0];  // x
  qvec_wxyz[2] = q_conv[1];  // y
  qvec_wxyz[3] = q_conv[2];  // z
  const std::array<double, 3> neg_t = {-pose.translation_xyz[0],
                                       -pose.translation_xyz[1],
                                       -pose.translation_xyz[2]};
  const std::array<double, 3> rotated = RotateVectorByQuaternion(q_conv, neg_t);
  tvec[0] = rotated[0];
  tvec[1] = rotated[1];
  tvec[2] = rotated[2];
}

struct RowCamera {
  int model_id = 0;
  std::vector<double> params;
};

// Canonical intrinsic_model + distortion_model -> native COLMAP camera record.
// FAILS CLOSED for any combination 8c cannot represent losslessly (P5/P12).
RowCamera CameraRecord(const ReconCamera& cam) {
  const auto& d = cam.distortion_coefficients;
  const bool valid_coefs =
      std::all_of(d.begin(), d.end(),
                  [](double v) { return std::isfinite(v); });
  if (!valid_coefs) {
    throw WriterError("non-finite distortion coefficient in camera " +
                      std::to_string(cam.camera_id));
  }
  if (cam.intrinsic_model == "pinhole" && cam.distortion_model == "none") {
    if (!d.empty()) {
      throw WriterError("pinhole camera " + std::to_string(cam.camera_id) +
                        " carries distortion coefficients; 8c refuses to "
                        "drop or reinterpret them (P5)");
    }
    return {kPinHoleModel, {cam.fx, cam.fy, cam.cx, cam.cy}};
  }
  if (cam.intrinsic_model == "opencv" &&
      cam.distortion_model == "opencv_radial") {
    if (d.size() != 4) {
      throw WriterError("opencv camera " + std::to_string(cam.camera_id) +
                        " must carry exactly 4 distortion coefficients "
                        "(k1, k2, p1, p2)");
    }
    return {kOpenCvModel,
            {cam.fx, cam.fy, cam.cx, cam.cy, d[0], d[1], d[2], d[3]}};
  }
  if (cam.intrinsic_model == "opencv_fisheye" &&
      cam.distortion_model == "opencv_fisheye") {
    if (d.size() != 4) {
      throw WriterError("opencv_fisheye camera " +
                        std::to_string(cam.camera_id) +
                        " must carry exactly 4 distortion coefficients "
                        "(k1, k2, k3, k4)");
    }
    return {kOpenCvFishEyeModel,
            {cam.fx, cam.fy, cam.cx, cam.cy, d[0], d[1], d[2], d[3]}};
  }
  throw WriterError("unsupported camera model '" + cam.intrinsic_model +
                    "' (distortion '" + cam.distortion_model +
                    "'); 8c supports pinhole/opencv/opencv_fisheye only "
                    "(P12-i)");
}

}  // namespace

void WriteColmapModel(
    const std::filesystem::path& model_dir,
    const spatial::core::Reconstruction& rec,
    const std::map<std::uint32_t, ColmapKeypoints>& keypoints_by_image_id) {
  if (rec.cameras.empty()) {
    throw WriterError("reconstruction has no cameras");
  }
  if (rec.images.empty()) {
    throw WriterError("reconstruction has no images");
  }

  // Duplicate native-id guards (determinism; a duplicated id breaks the
  // native model contract and any future round trip).
  {
    std::vector<std::uint32_t> camera_ids;
    camera_ids.reserve(rec.cameras.size());
    for (const ReconCamera& c : rec.cameras) camera_ids.push_back(c.camera_id);
    std::sort(camera_ids.begin(), camera_ids.end());
    if (std::adjacent_find(camera_ids.begin(), camera_ids.end()) !=
        camera_ids.end()) {
      throw WriterError("duplicate camera_id in reconstruction");
    }
  }
  {
    std::vector<std::uint32_t> image_ids;
    image_ids.reserve(rec.images.size());
    for (const ReconImage& i : rec.images) image_ids.push_back(i.image_id);
    std::sort(image_ids.begin(), image_ids.end());
    if (std::adjacent_find(image_ids.begin(), image_ids.end()) !=
        image_ids.end()) {
      throw WriterError("duplicate image_id in reconstruction");
    }
  }
  {
    std::vector<std::uint64_t> point_ids;
    point_ids.reserve(rec.points3D.size());
    for (const ReconPoint3D& p : rec.points3D) point_ids.push_back(p.point3d_id);
    std::sort(point_ids.begin(), point_ids.end());
    if (std::adjacent_find(point_ids.begin(), point_ids.end()) !=
        point_ids.end()) {
      throw WriterError("duplicate point3d_id in reconstruction");
    }
  }
  {
    std::map<std::string, std::uint32_t> names;
    for (const ReconImage& i : rec.images) {
      if (i.name.empty()) {
        throw WriterError("image " + std::to_string(i.image_id) +
                          " has an empty name; the native COLMAP image "
                          "identity requires one");
      }
      if (!names.emplace(i.name, i.image_id).second) {
        throw WriterError("duplicate image name '" + i.name +
                          "' (COLMAP identifies images by name)");
      }
    }
  }

  // Every camera must be representable; resolve the intrinsic model up-front so
  // the validation is complete before anything is written.
  std::map<std::uint32_t, RowCamera> camera_rows;
  for (const ReconCamera& c : rec.cameras) {
    if (c.width < 0 || c.height < 0) {
      throw WriterError("camera " + std::to_string(c.camera_id) +
                        " has a negative image dimension");
    }
    camera_rows.emplace(c.camera_id, CameraRecord(c));
  }

  // Resolve every image's keypoints and the track maps (image_id x point2d_idx
  // -> point3d_id). An image with no keypoint source fails closed.
  std::map<std::uint32_t, ColmapKeypoints> keypoints;
  std::map<std::uint32_t, std::map<std::uint32_t, std::uint64_t>>
      track_by_image;
  for (const ReconImage& img : rec.images) {
    const auto it = keypoints_by_image_id.find(img.image_id);
    if (it == keypoints_by_image_id.end()) {
      throw WriterError("no keypoints supplied for image " +
                        std::to_string(img.image_id) + " ('" + img.name +
                        "'); the native images.bin requires the full keypoint "
                        "list (P12)");
    }
    if (it->second.empty()) {
      throw WriterError("image " + std::to_string(img.image_id) + " ('" +
                        img.name +
                        "') has an empty keypoint list; 8c fails closed on a "
                        "degenerate native model");
    }
    for (const std::array<double, 2>& kp : it->second) {
      if (std::isnan(kp[0]) || std::isnan(kp[1])) {
        throw WriterError("non-finite keypoint in image " +
                          std::to_string(img.image_id) + " (P12-ii)");
      }
    }
    keypoints.emplace(img.image_id, it->second);
  }
  for (const ReconPoint3D& pt : rec.points3D) {
    for (const ReconPoint3D::TrackElement& te : pt.track) {
      const auto kp = keypoints.find(te.image_id);
      if (kp == keypoints.end()) {
        throw WriterError("track of point " + std::to_string(pt.point3d_id) +
                          " references unknown image " +
                          std::to_string(te.image_id));
      }
      if (te.point2d_idx < 0 ||
          static_cast<std::size_t>(te.point2d_idx) >= kp->second.size()) {
        throw WriterError("track of point " + std::to_string(pt.point3d_id) +
                          " references out-of-range point2d_idx " +
                          std::to_string(te.point2d_idx));
      }
      if (!track_by_image[te.image_id]
               .emplace(static_cast<std::uint32_t>(te.point2d_idx),
                        pt.point3d_id)
               .second) {
        throw WriterError("duplicate track element (image " +
                          std::to_string(te.image_id) + ", point2d_idx " +
                          std::to_string(te.point2d_idx) + ")");
      }
    }
  }

  // cameras.bin (sorted by camera_id).
  {
    ColmapBinWriter out(model_dir / "cameras.bin");
    if (!out.ok()) {
      throw WriterError("cannot open cameras.bin for writing");
    }
    std::vector<std::uint32_t> ids;
    ids.reserve(camera_rows.size());
    for (const auto& kv : camera_rows) ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());
    out.U64(ids.size());
    for (const std::uint32_t id : ids) {
      const ReconCamera* cam = nullptr;
      for (const ReconCamera& c : rec.cameras) {
        if (c.camera_id == id) {
          cam = &c;
          break;
        }
      }
      const RowCamera& row = camera_rows.at(id);
      out.U32(id);
      out.I32(row.model_id);
      out.U64(static_cast<std::uint64_t>(cam->width));
      out.U64(static_cast<std::uint64_t>(cam->height));
      for (const double p : row.params) out.F64(p);
    }
    out.Close();
  }

  // images.bin (sorted by image_id; point2D in index order).
  {
    ColmapBinWriter out(model_dir / "images.bin");
    if (!out.ok()) {
      throw WriterError("cannot open images.bin for writing");
    }
    std::vector<std::uint32_t> ids;
    ids.reserve(rec.images.size());
    for (const ReconImage& i : rec.images) ids.push_back(i.image_id);
    std::sort(ids.begin(), ids.end());
    out.U64(ids.size());
    for (const std::uint32_t id : ids) {
      const ReconImage* img = nullptr;
      for (const ReconImage& i : rec.images) {
        if (i.image_id == id) {
          img = &i;
          break;
        }
      }
      double qvec[4] = {};
      double tvec[3] = {};
      CanonicalPoseToColmap(img->pose, qvec, tvec);
      out.U32(img->image_id);
      out.F64(qvec[0]);
      out.F64(qvec[1]);
      out.F64(qvec[2]);
      out.F64(qvec[3]);
      out.F64(tvec[0]);
      out.F64(tvec[1]);
      out.F64(tvec[2]);
      out.U32(img->camera_id);
      out.String(img->name);
      const ColmapKeypoints& kpts = keypoints.at(img->image_id);
      out.U64(kpts.size());
      const auto& track = track_by_image[img->image_id];
      for (std::size_t idx = 0; idx < kpts.size(); ++idx) {
        out.F64(kpts[idx][0]);
        out.F64(kpts[idx][1]);
        const auto tit = track.find(static_cast<std::uint32_t>(idx));
        if (tit != track.end()) {
          out.I64(static_cast<std::int64_t>(tit->second));
        } else {
          out.I64(-1);  // unobserved
        }
      }
    }
    out.Close();
  }

  // points3D.bin (sorted by point3d_id).
  {
    ColmapBinWriter out(model_dir / "points3D.bin");
    if (!out.ok()) {
      throw WriterError("cannot open points3D.bin for writing");
    }
    std::vector<std::uint64_t> ids;
    ids.reserve(rec.points3D.size());
    for (const ReconPoint3D& p : rec.points3D) ids.push_back(p.point3d_id);
    std::sort(ids.begin(), ids.end());
    out.U64(ids.size());
    for (const std::uint64_t id : ids) {
      const ReconPoint3D* pt = nullptr;
      for (const ReconPoint3D& p : rec.points3D) {
        if (p.point3d_id == id) {
          pt = &p;
          break;
        }
      }
      out.U64(pt->point3d_id);
      out.F64(pt->xyz[0]);
      out.F64(pt->xyz[1]);
      out.F64(pt->xyz[2]);
      out.U8(pt->color[0]);
      out.U8(pt->color[1]);
      out.U8(pt->color[2]);
      out.F64(pt->error);
      out.U64(pt->track.size());
      for (const ReconPoint3D::TrackElement& te : pt->track) {
        out.U32(te.image_id);
        out.U32(static_cast<std::uint32_t>(te.point2d_idx));
      }
    }
    out.Close();
  }
}

}  // namespace spatial::adapters::colmap