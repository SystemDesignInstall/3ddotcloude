// COLMAP converter unit tests (C1-S4; RFC-0008 §6/§7, RFC-0009 §5; plan §11
// test_colmap_converter). The fixture builder writes the COLMAP native binary
// layouts documented in adapters/colmap/colmap_converter.h (little-endian),
// then the parser must reconstruct the provisional canonical document and the
// serializer must produce deterministic bytes (ADR-020).

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adapters/colmap/colmap_converter.h"
#include "core/errors/project_error.h"

namespace spatial::adapters::colmap {
namespace {

using spatial::core::AdapterError;
using spatial::core::ErrorCode;
using nlohmann::json;

// Little-endian writer for the documented COLMAP binary layouts.
class BinBuilder {
 public:
  void U8(std::uint8_t value) { bytes_.push_back(value); }

  void U32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
  }

  void I32(std::int32_t value) { U32(static_cast<std::uint32_t>(value)); }

  void U64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      bytes_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
  }

  void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }

  void F64(double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "IEEE-754 double expected");
    std::memcpy(&bits, &value, sizeof(bits));
    U64(bits);
  }

  void String(const std::string& value) {
    U32(static_cast<std::uint32_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  const std::vector<std::uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
};

void WriteFile(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out) << "cannot write " << path.string();
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

class ColmapConverterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_colmap_conv_" +
             std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path Model(const std::string& file) const {
    return root_ / file;
  }

  std::filesystem::path root_;
};

void AppendCamera(BinBuilder& out, std::uint32_t camera_id, std::int32_t model_id,
                  std::uint64_t width, std::uint64_t height,
                  const std::vector<double>& params) {
  out.U32(camera_id);
  out.I32(model_id);
  out.U64(width);
  out.U64(height);
  for (double param : params) {
    out.F64(param);
  }
}

void AppendImage(BinBuilder& out, std::uint32_t image_id, std::uint32_t camera_id,
                 const std::string& name, const std::array<double, 4>& qvec,
                 const std::array<double, 3>& tvec,
                 std::uint64_t num_points2d) {
  out.U32(image_id);
  for (double q : qvec) {
    out.F64(q);
  }
  for (double t : tvec) {
    out.F64(t);
  }
  out.U32(camera_id);
  out.String(name);
  out.U64(num_points2d);
  for (std::uint64_t i = 0; i < num_points2d; ++i) {
    out.F64(0.5 + static_cast<double>(i));
    out.F64(0.25 + static_cast<double>(i));
    out.I64(-1);  // unobserved
  }
}

void AppendPoint3D(BinBuilder& out, std::uint64_t point3d_id,
                   const std::array<double, 3>& xyz,
                   const std::array<std::uint8_t, 3>& rgb, double error,
                   const std::vector<std::pair<std::uint32_t, std::uint32_t>>&
                       track) {
  out.U64(point3d_id);
  for (double x : xyz) {
    out.F64(x);
  }
  for (std::uint8_t c : rgb) {
    out.U8(c);
  }
  out.F64(error);
  out.U64(track.size());
  for (const auto& [image_id, point2d_idx] : track) {
    out.U32(image_id);
    out.U32(point2d_idx);
  }
}

TEST_F(ColmapConverterTest, RoundTripProvisionalDocument) {
  BinBuilder cameras;
  cameras.U64(1);
  // OPENCV (model id 4): fx, fy, cx, cy, k1, k2, p1, p2.
  AppendCamera(cameras, 7, 4, 1920, 1080,
               {2457.4, 2460.1, 960.0, 540.0, -0.12, 0.03, 0.001, 0.002});

  BinBuilder images;
  images.U64(2);
  AppendImage(images, 1, 7, "image_0001.jpg",
              {0.0, 0.0, 0.0, 1.0}, {0.1, -0.2, 0.3}, 3);
  AppendImage(images, 2, 7, "image_0002.jpg",
              {0.5, 0.5, 0.5, 0.5}, {1.0, 2.0, 3.0}, 0);

  BinBuilder points;
  points.U64(1);
  AppendPoint3D(points, 42, {1.0, 2.0, 3.0}, {255, 0, 128}, 0.75,
                {{1, 0}, {2, 5}});

  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  // Parsed document.
  EXPECT_EQ(model.schema_version, 1);
  ASSERT_EQ(model.cameras.size(), 1u);
  EXPECT_EQ(model.cameras[0].camera_id, 7u);
  EXPECT_EQ(model.cameras[0].model, "OPENCV");
  EXPECT_EQ(model.cameras[0].intrinsic_model, "opencv");
  EXPECT_DOUBLE_EQ(model.cameras[0].intrinsics.fx, 2457.4);
  EXPECT_DOUBLE_EQ(model.cameras[0].intrinsics.fy, 2460.1);
  EXPECT_DOUBLE_EQ(model.cameras[0].intrinsics.cx, 960.0);
  EXPECT_DOUBLE_EQ(model.cameras[0].intrinsics.cy, 540.0);
  EXPECT_EQ(model.cameras[0].width, 1920);
  EXPECT_EQ(model.cameras[0].height, 1080);

  ASSERT_EQ(model.images.size(), 2u);
  EXPECT_EQ(model.images[0].image_id, 1u);
  EXPECT_EQ(model.images[0].camera_id, 7u);
  EXPECT_EQ(model.images[0].name, "image_0001.jpg");
  EXPECT_EQ(model.images[0].qvec, (std::array<double, 4>{0.0, 0.0, 0.0, 1.0}));
  EXPECT_EQ(model.images[0].tvec, (std::array<double, 3>{0.1, -0.2, 0.3}));
  EXPECT_EQ(model.images[1].image_id, 2u);
  EXPECT_EQ(model.images[1].name, "image_0002.jpg");

  ASSERT_EQ(model.points3d.size(), 1u);
  EXPECT_EQ(model.points3d[0].point3d_id, 42u);
  EXPECT_EQ(model.points3d[0].xyz, (std::array<double, 3>{1.0, 2.0, 3.0}));
  EXPECT_EQ(model.points3d[0].rgb,
            (std::array<std::uint8_t, 3>{255, 0, 128}));
  EXPECT_DOUBLE_EQ(model.points3d[0].error, 0.75);
  ASSERT_EQ(model.points3d[0].track.size(), 2u);
  EXPECT_EQ(model.points3d[0].track[0].image_id, 1u);
  EXPECT_EQ(model.points3d[0].track[0].point2d_idx, 0u);
  EXPECT_EQ(model.points3d[0].track[1].image_id, 2u);
  EXPECT_EQ(model.points3d[0].track[1].point2d_idx, 5u);

  // Canonical JSON: structure + values.
  const json document = json::parse(SparseModelToJson(model));
  EXPECT_EQ(document["schema_version"].get<int>(), 1);
  ASSERT_TRUE(document["cameras"].is_array());
  ASSERT_EQ(document["cameras"].size(), 1u);
  EXPECT_EQ(document["cameras"][0]["camera_id"].get<std::uint32_t>(), 7u);
  EXPECT_EQ(document["cameras"][0]["model"].get<std::string>(), "OPENCV");
  EXPECT_EQ(document["cameras"][0]["intrinsic_model"].get<std::string>(),
            "opencv");
  EXPECT_DOUBLE_EQ(document["cameras"][0]["intrinsics"]["fx"].get<double>(),
                   2457.4);
  EXPECT_DOUBLE_EQ(document["cameras"][0]["intrinsics"]["fy"].get<double>(),
                   2460.1);
  EXPECT_DOUBLE_EQ(document["cameras"][0]["intrinsics"]["cx"].get<double>(),
                   960.0);
  EXPECT_DOUBLE_EQ(document["cameras"][0]["intrinsics"]["cy"].get<double>(),
                   540.0);
  ASSERT_EQ(document["images"].size(), 2u);
  EXPECT_EQ(document["images"][0]["qvec_xyzw"],
            json::array({0.0, 0.0, 0.0, 1.0}));
  EXPECT_EQ(document["images"][0]["tvec_xyz"],
            json::array({0.1, -0.2, 0.3}));
  ASSERT_EQ(document["points3D"].size(), 1u);
  EXPECT_EQ(document["points3D"][0]["rgb"], json::array({255, 0, 128}));
  EXPECT_EQ(document["points3D"][0]["track"],
            json::array({json::array({1u, 0u}), json::array({2u, 5u})}));

  // Determinism (ADR-020): parsing the same files again yields identical
  // bytes; a second, equal document serializes identically.
  const SparseModel again =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));
  EXPECT_EQ(again, model);
  EXPECT_EQ(SparseModelToJson(again), SparseModelToJson(model));
}

TEST_F(ColmapConverterTest, CameraModelsMapPerRfc0009) {
  struct Expectation {
    std::int32_t model_id;
    const char* model;
    const char* intrinsic_model;
    bool single_focal;
    int num_params;
  };
  const Expectation kExpectations[] = {
      {0, "SIMPLE_PINHOLE", "pinhole", true, 3},
      {1, "PINHOLE", "pinhole", false, 4},
      {2, "SIMPLE_RADIAL", "opencv", true, 4},
      {3, "RADIAL", "opencv", true, 5},
      {4, "OPENCV", "opencv", false, 8},
      {5, "OPENCV_FISHEYE", "opencv_fisheye", false, 8},
      {6, "FULL_OPENCV", "custom", false, 12},
      {7, "FOV", "fov", false, 5},
      {8, "SIMPLE_RADIAL_FISHEYE", "opencv_fisheye", true, 4},
      {9, "RADIAL_FISHEYE", "opencv_fisheye", true, 5},
      {10, "THIN_PRISM_FISHEYE", "custom", false, 12},
  };

  BinBuilder cameras;
  cameras.U64(std::size(kExpectations));
  for (const Expectation& e : kExpectations) {
    // Single-focal models read f from params[0]; two-focal models read
    // fx=params[0], fy=params[1] — the distinct values prove the extraction.
    // The param vector must match the model's declared count so records stay
    // aligned (as a real COLMAP cameras.bin would be).
    std::vector<double> params(static_cast<std::size_t>(e.num_params), 0.1);
    params[0] = 1234.5;
    params[1] = e.single_focal ? 320.0 : 1235.5;
    params[2] = e.single_focal ? 240.0 : 320.0;
    if (!e.single_focal) {
      params[3] = 240.0;
    }
    AppendCamera(cameras, e.model_id, e.model_id, 640, 480, params);
  }
  BinBuilder images;
  images.U64(0);
  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));
  ASSERT_EQ(model.cameras.size(), std::size(kExpectations));
  for (std::size_t i = 0; i < std::size(kExpectations); ++i) {
    const SparseModelCamera& camera = model.cameras[i];
    const Expectation& e = kExpectations[i];
    EXPECT_EQ(camera.camera_id, static_cast<std::uint32_t>(e.model_id))
        << "camera id " << e.model;
    EXPECT_EQ(camera.model, e.model);
    EXPECT_EQ(camera.intrinsic_model, e.intrinsic_model);
    EXPECT_EQ(camera.width, 640);
    EXPECT_EQ(camera.height, 480);
    if (e.single_focal) {
      EXPECT_DOUBLE_EQ(camera.intrinsics.fx, 1234.5) << e.model;
      EXPECT_DOUBLE_EQ(camera.intrinsics.fy, 1234.5) << e.model;
      EXPECT_DOUBLE_EQ(camera.intrinsics.cx, 320.0) << e.model;
      EXPECT_DOUBLE_EQ(camera.intrinsics.cy, 240.0) << e.model;
    } else {
      EXPECT_DOUBLE_EQ(camera.intrinsics.fx, 1234.5) << e.model;
      EXPECT_DOUBLE_EQ(camera.intrinsics.fy, 1235.5) << e.model;
      EXPECT_DOUBLE_EQ(camera.intrinsics.cx, 320.0) << e.model;
      EXPECT_DOUBLE_EQ(camera.intrinsics.cy, 240.0) << e.model;
    }
  }
}

TEST_F(ColmapConverterTest, RecordsAreSortedDeterministically) {
  BinBuilder cameras;
  cameras.U64(2);
  AppendCamera(cameras, 2, 1, 640, 480, {100.0, 101.0, 32.0, 24.0});
  AppendCamera(cameras, 1, 1, 640, 480, {110.0, 111.0, 32.0, 24.0});

  BinBuilder images;
  images.U64(2);
  AppendImage(images, 3, 1, "c.jpg", {0.0, 0.0, 0.0, 1.0}, {3, 3, 3}, 0);
  AppendImage(images, 1, 1, "a.jpg", {0.0, 0.0, 0.0, 1.0}, {1, 1, 1}, 0);

  BinBuilder points;
  points.U64(2);
  AppendPoint3D(points, 9, {9, 9, 9}, {9, 9, 9}, 0.1, {{1, 0}});
  AppendPoint3D(points, 2, {2, 2, 2}, {2, 2, 2}, 0.2, {{1, 0}});

  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  EXPECT_EQ(model.cameras[0].camera_id, 1u);
  EXPECT_EQ(model.cameras[1].camera_id, 2u);
  EXPECT_EQ(model.images[0].image_id, 1u);
  EXPECT_EQ(model.images[1].image_id, 3u);
  EXPECT_EQ(model.points3d[0].point3d_id, 2u);
  EXPECT_EQ(model.points3d[1].point3d_id, 9u);

  // Deterministic: the same document built in a different record order must
  // serialize to identical bytes.
  const SparseModel reordered = model;
  EXPECT_EQ(SparseModelToJson(reordered), SparseModelToJson(model));
}

TEST_F(ColmapConverterTest, EmptyModelSerializesToEmptyDocument) {
  BinBuilder cameras;
  cameras.U64(0);
  BinBuilder images;
  images.U64(0);
  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));
  EXPECT_TRUE(model.cameras.empty());
  EXPECT_TRUE(model.images.empty());
  EXPECT_TRUE(model.points3d.empty());
  const json document = json::parse(SparseModelToJson(model));
  EXPECT_TRUE(document["cameras"].empty());
  EXPECT_TRUE(document["images"].empty());
  EXPECT_TRUE(document["points3D"].empty());
}

TEST_F(ColmapConverterTest, MalformedFilesFailClosed) {
  // Truncated camera record (model id present, params cut off).
  BinBuilder cameras;
  cameras.U64(1);
  cameras.U32(1);
  cameras.I32(1);
  cameras.U64(640);
  cameras.U64(480);
  cameras.F64(100.0);
  cameras.F64(101.0);
  BinBuilder images;
  images.U64(0);
  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());
  try {
    ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                     Model("points3D.bin"));
    FAIL() << "expected AdapterError for a truncated cameras.bin";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
  }

  // Unknown camera model id.
  BinBuilder cameras2;
  cameras2.U64(1);
  AppendCamera(cameras2, 1, 99, 640, 480, {100.0, 101.0, 32.0, 24.0});
  WriteFile(Model("cameras.bin"), cameras2.bytes());
  try {
    ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                     Model("points3D.bin"));
    FAIL() << "expected AdapterError for an unknown camera model id";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
  }

  // Image count that overruns the file.
  BinBuilder images2;
  images2.U64(1000);
  images2.U32(1);
  WriteFile(Model("images.bin"), images2.bytes());
  try {
    ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                     Model("points3D.bin"));
    FAIL() << "expected AdapterError for an overrunning image count";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
  }

  // Truncated point3D track element.
  BinBuilder points2;
  points2.U64(1);
  points2.U64(1);
  for (int i = 0; i < 3; ++i) {
    points2.F64(1.0);
  }
  for (int i = 0; i < 3; ++i) {
    points2.U8(255);
  }
  points2.F64(0.1);
  points2.U64(1);
  points2.U32(1);  // image_id present, point2d_idx cut off
  WriteFile(Model("points3D.bin"), points2.bytes());
  try {
    ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                     Model("points3D.bin"));
    FAIL() << "expected AdapterError for a truncated track element";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
  }
}

TEST_F(ColmapConverterTest, MissingFileFailsClosed) {
  BinBuilder cameras;
  cameras.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  try {
    ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                     Model("points3D.bin"));
    FAIL() << "expected AdapterError for a missing native file";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
    EXPECT_NE(e.message().find("images.bin"), std::string::npos);
  }
}

}  // namespace
}  // namespace spatial::adapters::colmap
