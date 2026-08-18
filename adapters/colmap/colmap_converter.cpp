#include "adapters/colmap/colmap_converter.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <map>

#include "core/errors/project_error.h"
#include "core/reconstruction/reconstruction.h"
#include "core/utils/fs.h"

namespace spatial::adapters::colmap {
namespace {

using spatial::core::AdapterError;
using spatial::core::ErrorCode;
using nlohmann::json;

const char* const kCamerasLabel = "cameras.bin";
const char* const kImagesLabel = "images.bin";
const char* const kPoints3dLabel = "points3D.bin";

// Throws an adapter error naming the offending file.
[[noreturn]] void Malformed(const std::string& file, const std::string& detail) {
  throw AdapterError(ErrorCode::kAdapterProcessFailed,
                     file + ": " + detail, {},
                     /*recoverable=*/false,
                     "The COLMAP sparse-model files are corrupt, truncated, or "
                     "from an incompatible COLMAP version; re-run the "
                     "reconstruction.");
}

// RFC-0009 §5 camera-model table (see colmap_converter.h). Indexed by COLMAP
// camera model id.
struct CameraModelInfo {
  const char* name;
  int num_params;
  const char* intrinsic_model;
  bool single_focal;
};

constexpr std::array<CameraModelInfo, 11> kCameraModels = {{
    {"SIMPLE_PINHOLE", 3, "pinhole", true},
    {"PINHOLE", 4, "pinhole", false},
    {"SIMPLE_RADIAL", 4, "opencv", true},
    {"RADIAL", 5, "opencv", true},
    {"OPENCV", 8, "opencv", false},
    {"OPENCV_FISHEYE", 8, "opencv_fisheye", false},
    {"FULL_OPENCV", 12, "custom", false},
    {"FOV", 5, "fov", false},
    {"SIMPLE_RADIAL_FISHEYE", 4, "opencv_fisheye", true},
    {"RADIAL_FISHEYE", 5, "opencv_fisheye", true},
    {"THIN_PRISM_FISHEYE", 12, "custom", false},
}};

const CameraModelInfo* ModelInfo(int model_id) {
  if (model_id >= 0 &&
      static_cast<std::size_t>(model_id) < kCameraModels.size()) {
    return &kCameraModels[static_cast<std::size_t>(model_id)];
  }
  return nullptr;
}

// Bounds-checked little-endian reader over a whole-file byte buffer. Every
// read past the end is a malformed-file error (never UB, never a partial
// document).
class BinaryReader {
 public:
  BinaryReader(std::vector<std::uint8_t> bytes, std::string file)
      : bytes_(std::move(bytes)), file_(std::move(file)) {}

  std::uint8_t U8() {
    Need(1);
    return bytes_[pos_++];
  }

  std::uint32_t U32() {
    Need(4);
    const std::uint32_t value =
        static_cast<std::uint32_t>(bytes_[pos_]) |
        (static_cast<std::uint32_t>(bytes_[pos_ + 1]) << 8) |
        (static_cast<std::uint32_t>(bytes_[pos_ + 2]) << 16) |
        (static_cast<std::uint32_t>(bytes_[pos_ + 3]) << 24);
    pos_ += 4;
    return value;
  }

  std::int32_t I32() { return static_cast<std::int32_t>(U32()); }

  std::int64_t I64() {
    const std::uint64_t lo = U32();
    const std::uint64_t hi = U32();
    return static_cast<std::int64_t>(lo | (hi << 32));
  }

  std::uint64_t U64() {
    const std::uint64_t lo = U32();
    const std::uint64_t hi = U32();
    return lo | (hi << 32);
  }

  double F64() {
    const std::uint64_t bits = U64();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  std::string String() {
    const std::uint32_t len = U32();
    Need(len);
    std::string value(
        reinterpret_cast<const char*>(bytes_.data() + pos_),
        static_cast<std::size_t>(len));
    pos_ += static_cast<std::size_t>(len);
    return value;
  }

  void Skip(std::size_t count) {
    Need(count);
    pos_ += count;
  }

  std::size_t Remaining() const { return bytes_.size() - pos_; }

 private:
  void Need(std::size_t count) const {
    if (pos_ > bytes_.size() || count > bytes_.size() - pos_) {
      Malformed(file_, "truncated or corrupt binary data");
    }
  }

  std::vector<std::uint8_t> bytes_;
  std::string file_;
  std::size_t pos_ = 0;
};

std::vector<std::uint8_t> ReadNative(const std::filesystem::path& path,
                                     const char* label) {
  try {
    return spatial::core::fs::ReadFile(path);
  } catch (const std::exception& e) {
    throw AdapterError(ErrorCode::kAdapterProcessFailed,
                       std::string(label) + " unreadable: " +
                           path.string() + ": " + e.what(),
                       {}, /*recoverable=*/false,
                       "Provide the complete COLMAP sparse-model output; the "
                       "file is missing or unreadable.");
  }
}

SparseModelCamera ParseCamera(BinaryReader& reader) {
  SparseModelCamera camera;
  camera.camera_id = reader.U32();
  const int model_id = reader.I32();
  const CameraModelInfo* info = ModelInfo(model_id);
  if (info == nullptr) {
    Malformed(kCamerasLabel,
              "unknown camera model id " + std::to_string(model_id));
  }
  camera.model = info->name;
  camera.intrinsic_model = info->intrinsic_model;
  camera.model_id = model_id;
  camera.width = static_cast<std::int64_t>(reader.U64());
  camera.height = static_cast<std::int64_t>(reader.U64());
  std::vector<double> params(static_cast<std::size_t>(info->num_params));
  for (double& param : params) {
    param = reader.F64();
  }
  camera.raw_params = params;
  if (info->single_focal) {
    camera.intrinsics.fx = params[0];
    camera.intrinsics.fy = params[0];
    camera.intrinsics.cx = params[1];
    camera.intrinsics.cy = params[2];
  } else {
    camera.intrinsics.fx = params[0];
    camera.intrinsics.fy = params[1];
    camera.intrinsics.cx = params[2];
    camera.intrinsics.cy = params[3];
  }
  return camera;
}

std::vector<SparseModelCamera> ParseCameras(std::vector<std::uint8_t> bytes) {
  BinaryReader reader(std::move(bytes), kCamerasLabel);
  const std::uint64_t num = reader.U64();
  if (num > reader.Remaining()) {
    Malformed(kCamerasLabel, "camera count exceeds the file size");
  }
  std::vector<SparseModelCamera> cameras;
  cameras.reserve(static_cast<std::size_t>(num));
  for (std::uint64_t i = 0; i < num; ++i) {
    cameras.push_back(ParseCamera(reader));
  }
  return cameras;
}

SparseModelImage ParseImage(BinaryReader& reader) {
  SparseModelImage image;
  image.image_id = reader.U32();
  for (double& q : image.qvec) {
    q = reader.F64();
  }
  for (double& t : image.tvec) {
    t = reader.F64();
  }
  image.camera_id = reader.U32();
  image.name = reader.String();
  const std::uint64_t num_points2d = reader.U64();
  for (std::uint64_t i = 0; i < num_points2d; ++i) {
    reader.Skip(sizeof(double) * 2 + sizeof(std::int64_t));
  }
  return image;
}

std::vector<SparseModelImage> ParseImages(std::vector<std::uint8_t> bytes) {
  BinaryReader reader(std::move(bytes), kImagesLabel);
  const std::uint64_t num = reader.U64();
  if (num > reader.Remaining()) {
    Malformed(kImagesLabel, "image count exceeds the file size");
  }
  std::vector<SparseModelImage> images;
  images.reserve(static_cast<std::size_t>(num));
  for (std::uint64_t i = 0; i < num; ++i) {
    images.push_back(ParseImage(reader));
  }
  return images;
}

SparseModelPoint ParsePoint3D(BinaryReader& reader) {
  SparseModelPoint point;
  point.point3d_id = reader.U64();
  for (double& x : point.xyz) {
    x = reader.F64();
  }
  for (std::uint8_t& c : point.rgb) {
    c = reader.U8();
  }
  point.error = reader.F64();
  const std::uint64_t track_len = reader.U64();
  point.track.reserve(static_cast<std::size_t>(track_len));
  for (std::uint64_t i = 0; i < track_len; ++i) {
    SparseModelTrackElement element;
    element.image_id = reader.U32();
    element.point2d_idx = reader.U32();
    point.track.push_back(element);
  }
  return point;
}

std::vector<SparseModelPoint> ParsePoints3D(std::vector<std::uint8_t> bytes) {
  BinaryReader reader(std::move(bytes), kPoints3dLabel);
  const std::uint64_t num = reader.U64();
  if (num > reader.Remaining()) {
    Malformed(kPoints3dLabel, "point3D count exceeds the file size");
  }
  std::vector<SparseModelPoint> points;
  points.reserve(static_cast<std::size_t>(num));
  for (std::uint64_t i = 0; i < num; ++i) {
    points.push_back(ParsePoint3D(reader));
  }
  return points;
}

bool LessCameraById(const SparseModelCamera& a, const SparseModelCamera& b) {
  return a.camera_id < b.camera_id;
}

bool LessImageById(const SparseModelImage& a, const SparseModelImage& b) {
  return a.image_id < b.image_id;
}

bool LessPointById(const SparseModelPoint& a, const SparseModelPoint& b) {
  return a.point3d_id < b.point3d_id;
}

}  // namespace

SparseModel ParseSparseModel(const std::filesystem::path& cameras_bin,
                             const std::filesystem::path& images_bin,
                             const std::filesystem::path& points3d_bin) {
  SparseModel model;
  model.cameras = ParseCameras(ReadNative(cameras_bin, kCamerasLabel));
  model.images = ParseImages(ReadNative(images_bin, kImagesLabel));
  model.points3d = ParsePoints3D(ReadNative(points3d_bin, kPoints3dLabel));

  // Deterministic serialization: the document is a pure function of the
  // model, regardless of the native record order (ADR-020).
  std::sort(model.cameras.begin(), model.cameras.end(), LessCameraById);
  std::sort(model.images.begin(), model.images.end(), LessImageById);
  std::sort(model.points3d.begin(), model.points3d.end(), LessPointById);
  return model;
}

std::string SparseModelToJson(const SparseModel& model) {
  json document;
  document["schema_version"] = model.schema_version;

  json cameras = json::array();
  for (const SparseModelCamera& camera : model.cameras) {
    json entry;
    entry["camera_id"] = camera.camera_id;
    entry["model"] = camera.model;
    entry["intrinsic_model"] = camera.intrinsic_model;
    json intrinsics;
    intrinsics["fx"] = camera.intrinsics.fx;
    intrinsics["fy"] = camera.intrinsics.fy;
    intrinsics["cx"] = camera.intrinsics.cx;
    intrinsics["cy"] = camera.intrinsics.cy;
    entry["intrinsics"] = std::move(intrinsics);
    entry["width"] = camera.width;
    entry["height"] = camera.height;
    cameras.push_back(std::move(entry));
  }
  document["cameras"] = std::move(cameras);

  json images = json::array();
  for (const SparseModelImage& image : model.images) {
    json entry;
    entry["image_id"] = image.image_id;
    entry["camera_id"] = image.camera_id;
    entry["name"] = image.name;
    entry["qvec_xyzw"] = {image.qvec[0], image.qvec[1], image.qvec[2],
                          image.qvec[3]};
    entry["tvec_xyz"] = {image.tvec[0], image.tvec[1], image.tvec[2]};
    images.push_back(std::move(entry));
  }
  document["images"] = std::move(images);

  json points = json::array();
  for (const SparseModelPoint& point : model.points3d) {
    json entry;
    entry["point3D_id"] = point.point3d_id;
    entry["xyz"] = {point.xyz[0], point.xyz[1], point.xyz[2]};
    entry["rgb"] = {static_cast<int>(point.rgb[0]),
                    static_cast<int>(point.rgb[1]),
                    static_cast<int>(point.rgb[2])};
    entry["error"] = point.error;
    json track = json::array();
    for (const SparseModelTrackElement& element : point.track) {
      track.push_back({element.image_id, element.point2d_idx});
    }
    entry["track"] = std::move(track);
    points.push_back(std::move(entry));
  }
  document["points3D"] = std::move(points);

  return document.dump();
}

// ---------------------------------------------------------------------------
// P2.5: Canonical Reconstruction v2
// ---------------------------------------------------------------------------

namespace {

// COLMAP camera model id → distortion model string (RFC-0009 §5).
const char* DistortionModelForId(int model_id) {
  switch (model_id) {
    case 0:  // SIMPLE_PINHOLE
    case 1:  // PINHOLE
      return "none";
    case 2:  // SIMPLE_RADIAL
    case 3:  // RADIAL
    case 4:  // OPENCV
      return "opencv_radial";
    case 5:  // OPENCV_FISHEYE
    case 8:  // SIMPLE_RADIAL_FISHEYE
    case 9:  // RADIAL_FISHEYE
      return "opencv_fisheye";
    case 6:   // FULL_OPENCV
    case 7:   // FOV
    case 10:  // THIN_PRISM_FISHEYE
    default:
      return "custom";
  }
}

// Extract distortion coefficients from the raw COLMAP parameter vector.
std::vector<double> ExtractDistortion(const SparseModelCamera& cam) {
  const int mid = cam.model_id;
  const auto& p = cam.raw_params;
  const CameraModelInfo* info = ModelInfo(mid);
  if (info == nullptr) return {};
  const bool sf = info->single_focal;

  if (sf) {
    // single_focal: p[0]=f, p[1]=cx, p[2]=cy, distortion at [3..]
    switch (mid) {
      case 2:  // SIMPLE_RADIAL: k1
        if (p.size() > 3) return {p[3]};
        return {};
      case 3:  // RADIAL: k1, k2
        if (p.size() > 4) return {p[3], p[4]};
        return {};
      case 8:  // SIMPLE_RADIAL_FISHEYE: k1
        if (p.size() > 3) return {p[3]};
        return {};
      case 9:  // RADIAL_FISHEYE: k1, k2
        if (p.size() > 4) return {p[3], p[4]};
        return {};
      default:
        return {};
    }
  }
  // non-single_focal: p[0]=fx, [1]=fy, [2]=cx, [3]=cy, distortion at [4..]
  switch (mid) {
    case 1:  // PINHOLE: none
      return {};
    case 4: {  // OPENCV: k1, k2, p1, p2
      if (p.size() >= 8) return {p[4], p[5], p[6], p[7]};
      return {};
    }
    case 5: {  // OPENCV_FISHEYE: k1, k2, k3, k4
      if (p.size() >= 8) return {p[4], p[5], p[6], p[7]};
      return {};
    }
    case 7: {  // FOV: omega
      if (p.size() > 4) return {p[4]};
      return {};
    }
    default:
      return {};
  }
}

// Conjugate of a unit quaternion: q* = (-x, -y, -z, w).
void ConjugateQuaternion(const std::array<double, 4>& q,
                         std::array<double, 4>& out) {
  out[0] = -q[0];
  out[1] = -q[1];
  out[2] = -q[2];
  out[3] =  q[3];
}

// Multiply two quaternions: result = a * b (scalar-last convention).
void MultiplyQuaternion(const std::array<double, 4>& a,
                        const std::array<double, 4>& b,
                        std::array<double, 4>& out) {
  out[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
  out[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
  out[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
  out[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
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

// Convert COLMAP camera-to-world pose to T_reconstruction_camera
// (world-to-camera): R_cw = R_wc^T, t_cw = -R_wc^T * t_wc.
void InvertColmapPose(const std::array<double, 4>& qvec_wxyz,
                      const std::array<double, 3>& tvec,
                      std::array<double, 4>& rot_xyzw,
                      std::array<double, 3>& trans) {
  // COLMAP qvec is (w, x, y, z). Convert to our (x, y, z, w).
  std::array<double, 4> q_wxyz = {qvec_wxyz[1], qvec_wxyz[2],
                                   qvec_wxyz[3], qvec_wxyz[0]};
  // Conjugate = inverse rotation.
  std::array<double, 4> q_inv;
  ConjugateQuaternion(q_wxyz, q_inv);
  // t_cw = -R_wc^T * t_wc = -R_wc^{-1} * t_wc = -(q_inv * t_wc * q_inv*).
  std::array<double, 3> neg_t = {-tvec[0], -tvec[1], -tvec[2]};
  std::array<double, 3> rotated;
  RotateVectorByQuaternion(q_inv, neg_t, rotated);
  // Output.
  rot_xyzw = q_inv;  // already (x, y, z, w)
  trans = rotated;
}

}  // namespace

using core::Reconstruction;
using core::ReconCamera;
using core::ReconImage;
using core::ReconPoint3D;
using core::ReconstructionProvenance;

spatial::core::Reconstruction SparseModelToReconstruction(
    const SparseModel& model,
    const std::string& reconstruction_id,
    const std::string& scene_id,
    const std::vector<std::string>& session_ids,
    const std::string& coordinate_frame,
    const ReconstructionProvenanceInfo& prov_info,
    const std::map<std::string, std::string>& frame_id_map) {

  Reconstruction rec;
  rec.reconstruction_id = reconstruction_id;
  rec.scene_id = scene_id;
  rec.session_ids = session_ids;
  rec.coordinate_frame = coordinate_frame;
  rec.status = "succeeded";

  // Provenance (D-CRM-11).
  ReconstructionProvenance& prov = rec.provenance;
  prov.backend.name = prov_info.backend_name;
  prov.backend.version = prov_info.backend_version;
  prov.backend.adapter_version = prov_info.adapter_version;
  prov.configuration_hash = prov_info.configuration_hash;
  prov.input_artifact_hashes = prov_info.input_artifact_hashes;
  prov.engine_version = prov_info.engine_version;
  prov.engine_commit = prov_info.engine_commit;
  prov.git_commit = prov_info.git_commit;
  prov.started_at_ns = prov_info.started_at_ns;
  prov.finished_at_ns = prov_info.finished_at_ns;
  prov.duration_ns = prov_info.duration_ns;

  // Cameras (D-CRM-04, D-CRM-05).
  rec.cameras.reserve(model.cameras.size());
  for (const SparseModelCamera& src : model.cameras) {
    ReconCamera cam;
    cam.camera_id = src.camera_id;
    cam.width = src.width;
    cam.height = src.height;
    cam.intrinsic_model = src.intrinsic_model;
    cam.fx = src.intrinsics.fx;
    cam.fy = src.intrinsics.fy;
    cam.cx = src.intrinsics.cx;
    cam.cy = src.intrinsics.cy;
    cam.distortion_model = DistortionModelForId(src.model_id);
    cam.distortion_coefficients = ExtractDistortion(src);
    rec.cameras.push_back(std::move(cam));
  }

  // Images (D-CRM-20). COLMAP outputs only registered images; all are
  // detected=true. frame_id is resolved from the optional frame_id_map.
  rec.images.reserve(model.images.size());
  for (const SparseModelImage& src : model.images) {
    ReconImage img;
    img.image_id = src.image_id;
    img.camera_id = src.camera_id;
    img.name = src.name;
    img.detected = true;

    // Frame UUID mapping.
    auto it = frame_id_map.find(src.name);
    if (it != frame_id_map.end()) {
      img.frame_id = it->second;
    }
    // If not found, frame_id remains empty (caller resolves later).

    // COLMAP qvec (w,x,y,z) → canonical (x,y,z,w), then invert for
    // T_reconstruction_camera (D-CRM-01, D-CRM-02).
    InvertColmapPose(src.qvec, src.tvec,
                     img.pose.rotation_xyzw,
                     img.pose.translation_xyz);

    rec.images.push_back(std::move(img));
  }

  // 3D Points (D-CRM-08, D-CRM-16).
  rec.points3D.reserve(model.points3d.size());
  for (const SparseModelPoint& src : model.points3d) {
    ReconPoint3D pt;
    pt.point3d_id = src.point3d_id;
    pt.xyz = src.xyz;
    pt.color = src.rgb;
    pt.error = src.error;
    pt.track.reserve(src.track.size());
    for (const SparseModelTrackElement& te : src.track) {
      ReconPoint3D::TrackElement out_te;
      out_te.image_id = te.image_id;
      out_te.point2d_idx = static_cast<std::int32_t>(te.point2d_idx);
      pt.track.push_back(out_te);
    }
    rec.points3D.push_back(std::move(pt));
  }

  return rec;
}

// ---------------------------------------------------------------------------
// ReconstructionToJson: canonical JSON serialization (ADR-020).
// ---------------------------------------------------------------------------

std::string ReconstructionToJson(const spatial::core::Reconstruction& rec) {
  json doc;

  doc["schema_version"] = 2;
  doc["reconstruction_id"] = rec.reconstruction_id;
  doc["scene_id"] = rec.scene_id;

  json sids = json::array();
  for (const auto& sid : rec.session_ids) sids.push_back(sid);
  doc["session_ids"] = std::move(sids);

  doc["coordinate_frame"] = rec.coordinate_frame;
  if (!rec.status.empty()) doc["status"] = rec.status;
  doc["created_at_ns"] = rec.created_at_ns;

  // Provenance.
  json prov;
  json backend;
  backend["name"] = rec.provenance.backend.name;
  backend["version"] = rec.provenance.backend.version;
  backend["adapter_version"] = rec.provenance.backend.adapter_version;
  prov["backend"] = std::move(backend);
  prov["configuration_hash"] = rec.provenance.configuration_hash;
  json iah = json::array();
  for (const auto& h : rec.provenance.input_artifact_hashes) iah.push_back(h);
  prov["input_artifact_hashes"] = std::move(iah);
  prov["engine_version"] = rec.provenance.engine_version;
  prov["engine_commit"] = rec.provenance.engine_commit;
  prov["git_commit"] = rec.provenance.git_commit;
  prov["started_at_ns"] = rec.provenance.started_at_ns;
  prov["finished_at_ns"] = rec.provenance.finished_at_ns;
  prov["duration_ns"] = rec.provenance.duration_ns;
  doc["provenance"] = std::move(prov);

  // Cameras.
  json cameras = json::array();
  for (const ReconCamera& c : rec.cameras) {
    json entry;
    entry["camera_id"] = c.camera_id;
    entry["width"] = c.width;
    entry["height"] = c.height;
    entry["intrinsic_model"] = c.intrinsic_model;
    entry["fx"] = c.fx;
    entry["fy"] = c.fy;
    entry["cx"] = c.cx;
    entry["cy"] = c.cy;
    entry["distortion_model"] = c.distortion_model;
    entry["distortion_coefficients"] = c.distortion_coefficients;
    if (!c.calibration_ref.empty()) entry["calibration_ref"] = c.calibration_ref;
    cameras.push_back(std::move(entry));
  }
  doc["cameras"] = std::move(cameras);

  // Images.
  json images = json::array();
  for (const ReconImage& img : rec.images) {
    json entry;
    entry["image_id"] = img.image_id;
    entry["camera_id"] = img.camera_id;
    if (!img.frame_id.empty()) entry["frame_id"] = img.frame_id;
    entry["name"] = img.name;
    json pose;
    pose["rotation_xyzw"] = {img.pose.rotation_xyzw[0],
                             img.pose.rotation_xyzw[1],
                             img.pose.rotation_xyzw[2],
                             img.pose.rotation_xyzw[3]};
    pose["translation_xyz"] = {img.pose.translation_xyz[0],
                               img.pose.translation_xyz[1],
                               img.pose.translation_xyz[2]};
    entry["pose"] = std::move(pose);
    entry["detected"] = img.detected;
    images.push_back(std::move(entry));
  }
  doc["images"] = std::move(images);

  // Points3D.
  json points = json::array();
  for (const ReconPoint3D& pt : rec.points3D) {
    json entry;
    entry["point3d_id"] = pt.point3d_id;
    entry["xyz"] = {pt.xyz[0], pt.xyz[1], pt.xyz[2]};
    entry["color"] = {pt.color[0], pt.color[1], pt.color[2]};
    entry["error"] = pt.error;
    json track = json::array();
    for (const ReconPoint3D::TrackElement& te : pt.track) {
      track.push_back({{"image_id", te.image_id},
                       {"point2d_idx", te.point2d_idx}});
    }
    entry["track"] = std::move(track);
    points.push_back(std::move(entry));
  }
  doc["points3D"] = std::move(points);

  return doc.dump();
}

}  // namespace spatial::adapters::colmap
