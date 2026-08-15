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

#include "core/errors/project_error.h"
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
  camera.width = static_cast<std::int64_t>(reader.U64());
  camera.height = static_cast<std::int64_t>(reader.U64());
  std::vector<double> params(static_cast<std::size_t>(info->num_params));
  for (double& param : params) {
    param = reader.F64();
  }
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

}  // namespace spatial::adapters::colmap
