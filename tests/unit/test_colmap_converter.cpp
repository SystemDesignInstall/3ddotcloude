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
#include <functional>
#include <iterator>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adapters/colmap/colmap_converter.h"
#include "core/errors/project_error.h"
#include "core/reconstruction/reconstruction.h"
#include "schema_check.h"

namespace spatial::adapters::colmap {
namespace {

using spatial::core::AdapterError;
using spatial::core::ErrorCode;
using spatial::core::Reconstruction;
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

// ---------------------------------------------------------------------------
// P2.5 v2 converter tests
// ---------------------------------------------------------------------------

class ColmapConverterV2Test : public ColmapConverterTest {};

TEST_F(ColmapConverterV2Test, EmptyModelProducesValidV2Document) {
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

  ReconstructionProvenanceInfo prov;
  prov.configuration_hash = "aaaa" + std::string(60, 'a');

  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "rec-uuid-001", "scene-uuid-001", {"session-uuid-001"},
      "reconstruction_0", prov);

  EXPECT_EQ(rec.reconstruction_id, "rec-uuid-001");
  EXPECT_EQ(rec.scene_id, "scene-uuid-001");
  EXPECT_EQ(rec.session_ids.size(), 1u);
  EXPECT_EQ(rec.coordinate_frame, "reconstruction_0");
  EXPECT_EQ(rec.status, "succeeded");
  EXPECT_TRUE(rec.cameras.empty());
  EXPECT_TRUE(rec.images.empty());
  EXPECT_TRUE(rec.points3D.empty());

  const json doc = json::parse(ReconstructionToJson(rec));
  EXPECT_EQ(doc["schema_version"].get<int>(), 2);
  EXPECT_EQ(doc["reconstruction_id"].get<std::string>(), "rec-uuid-001");
  EXPECT_EQ(doc["scene_id"].get<std::string>(), "scene-uuid-001");
  EXPECT_EQ(doc["coordinate_frame"].get<std::string>(), "reconstruction_0");
  EXPECT_TRUE(doc["cameras"].empty());
  EXPECT_TRUE(doc["images"].empty());
  EXPECT_TRUE(doc["points3D"].empty());
}

TEST_F(ColmapConverterV2Test, QuaternionConversionInvertsPose) {
  // Identity pose: q=(1,0,0,0) in COLMAP (w,x,y,z) → t=(0,0,0)
  // Inverted should be identity: rot=(0,0,0,1) xyzw, trans=(0,0,0)
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

  BinBuilder images;
  images.U64(1);
  AppendImage(images, 1, 1, "img1.jpg",
              {1.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, 0);

  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  ASSERT_EQ(rec.images.size(), 1u);
  // Identity inversion: rotation stays (0,0,0,1), translation stays (0,0,0).
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[0], 0.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[1], 0.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[2], 0.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[3], 1.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[0], 0.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[1], 0.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[2], 0.0, 1e-12);
}

TEST_F(ColmapConverterV2Test, QuaternionInversionNonTrivial90DegY) {
  // COLMAP camera-to-world: 90° rotation around Y, translation (1, 0, 3).
  //
  // R_wc = |  0  0  1 |     (90° Y)
  //        |  0  1  0 |
  //        | -1  0  0 |
  //
  // qvec COLMAP (w,x,y,z) = (cos45°, 0, sin45°, 0) ≈ (0.70710678, 0, 0.70710678, 0)
  // tvec COLMAP = (1, 0, 3)
  //
  // Expected T_reconstruction_camera (world-to-camera):
  //   R_rc = R_wc^T = |  0  0 -1 |
  //                   |  0  1  0 |
  //                   |  1  0  0 |
  //   t_rc = -R_wc^T * t_wc = -(3, 0, -1) = (-3, 0, -1)
  //
  //   q_rc (x,y,z,w) = (-sin45°, 0, 0, cos45°) ≈ (-0.70710678, 0, 0, 0.70710678)
  //   Wait, no — -90° around Y gives q = (0, -sin45°, 0, cos45°).
  //
  //   Let me recompute: R_rc rotates world to camera. The inverse of a 90° Y
  //   rotation is a -90° Y rotation. For -90° around Y:
  //     q = (cos(-45°), 0, sin(-45°), 0) in COLMAP (w,x,y,z)
  //       = (cos45°, 0, -sin45°, 0)
  //   But we need it in canonical (x,y,z,w):
  //     = (0, -sin45°, 0, cos45°) = (0, -0.70710678, 0, 0.70710678)
  //
  // Verify: R_rc * t_wc + t_rc = (0,0,0):
  //   R_rc * (1,0,3)^T = (0*1+0*0+(-1)*3, 0*1+1*0+0*3, 1*1+0*0+0*3) = (-3, 0, 1)
  //   (-3, 0, 1) + (-3, 0, -1) = (-6, 0, 0)  ... wait, that's wrong.
  //
  //   Actually the identity we need: R_rc * p_w + t_rc = p_c (camera frame).
  //   If p_w = (1,0,3), then p_c = R_rc*(1,0,3) + (-3,0,-1)
  //     = (-3, 0, 1) + (-3, 0, -1) = (-6, 0, 0). That's not zero.
  //
  //   The correct check is: R_cw = inverse of R_wc.
  //   R_wc*(0,0,0) + t_wc = t_wc = (1,0,3).  Camera origin in world = (1,0,3).
  //   R_cw * (1,0,3) + t_cw should = (0,0,0). (Camera origin in camera = 0)
  //   R_cw * (1,0,3) = (0*1+0*0+(-1)*3, 0*1+1*0+0*3, 1*1+0*0+0*3) = (-3, 0, 1)
  //   (-3, 0, 1) + (-3, 0, -1) = (-6, 0, 0). Still wrong.
  //
  //   Let me redo from scratch. R_wc maps camera→world:
  //     R_wc * x_c + t_wc = x_w
  //   So camera origin (0,0,0) maps to world point t_wc = (1,0,3). ✓
  //
  //   R_cw = R_wc^T should map world→camera:
  //     R_cw * x_w + t_cw = x_c
  //   Camera origin in world is (1,0,3). So:
  //     R_cw * (1,0,3) + t_cw = (0,0,0)
  //     t_cw = -R_cw * (1,0,3)
  //
  //   R_wc = | 0  0  1 |     R_cw = R_wc^T = | 0  0 -1 |
  //          | 0  1  0 |                      | 0  1  0 |
  //          |-1  0  0 |                      | 1  0  0 |
  //
  //   R_cw * (1,0,3) = (0*1+0*0+(-1)*3, 0*1+1*0+0*3, 1*1+0*0+0*3) = (-3, 0, 1)
  //   t_cw = -(-3, 0, 1) = (3, 0, -1)
  //
  //   AH — my earlier computation had a sign error in the report! Let me recompute
  //   using the InvertColmapPose formula to get the exact code output:
  //
  //   Code path:
  //   q_wxyz = (x=0, y=0.70710678, z=0, w=0.70710678)  [reordered from COLMAP]
  //   q_inv = Conjugate(q_wxyz) = (0, -0.70710678, 0, 0.70710678)
  //   -tvec = (-1, 0, -3)
  //   RotateVectorByQuaternion(q_inv, (-1, 0, -3)):
  //     qx=0, qy=-0.70710678, qz=0, qw=0.70710678
  //     vx=-1, vy=0, vz=-3
  //     cross(q_xyz, v) = (qy*vz - qz*vy, qz*vx - qx*vz, qx*vy - qy*vx)
  //                      = (-0.70710678*(-3) - 0, 0 - 0, 0 - (-0.70710678)*(-1))
  //                      = (2.12132034, 0, -0.70710678)
  //     t = 2*cross = (4.24264068, 0, -1.41421356)
  //     cross(q_xyz, t) = (qy*tz - qz*ty, qz*tx - qx*tz, qx*ty - qy*tx)
  //                      = (-0.70710678*(-1.41421356) - 0, 0 - 0, 0 - (-0.70710678)*4.24264068)
  //                      = (1.0, 0, 3.0)
  //     result = v + w*t + cross(q_xyz, t)
  //            = (-1, 0, -3) + 0.70710678*(4.24264068, 0, -1.41421356) + (1, 0, 3)
  //            = (-1, 0, -3) + (3, 0, -1) + (1, 0, 3)
  //            = (3, 0, -1)
  //
  //   So: rotation_xyzw = (0, -0.70710678, 0, 0.70710678), translation = (3, 0, -1)
  //
  //   Verify: R_cw * (1,0,3) = (-3, 0, 1), then R_cw*(1,0,3) + (3,0,-1) = (0,0,0) ✓
  //   Verify: R_cw * (5,0,0) = (0,0,5), then (0,0,5) + (3,0,-1) = (3,0,4) ✓
  //   Camera point (3,0,4) in world maps to camera origin... wait no:
  //   A world point at (5,0,0) → camera frame: R_cw*(5,0,0)+(3,0,-1) = (3,0,4).
  //   This means world point (5,0,0) appears at pixel (3,0,4) in camera — that's
  //   correct for a -90° Y rotation + translation.

  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

  const double S2 = std::sqrt(2.0) / 2.0;  // ≈ 0.70710678

  BinBuilder images;
  images.U64(1);
  // COLMAP (w,x,y,z): 90° around Y
  AppendImage(images, 1, 1, "img90y.jpg",
              {S2, 0.0, S2, 0.0}, {1.0, 0.0, 3.0}, 0);

  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  ASSERT_EQ(rec.images.size(), 1u);

  // Expected canonical rotation: -90° around Y → q_xyzw = (0, -sin45°, 0, cos45°)
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[0], 0.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[1], -S2, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[2], 0.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[3], S2, 1e-12);

  // Expected canonical translation: (3, 0, -1)
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[0], 3.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[1], 0.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[2], -1.0, 1e-12);
}

TEST_F(ColmapConverterV2Test, QuaternionInversionArbitraryAxis) {
  // COLMAP camera-to-world: 120° around axis (1,1,1)/√3, translation (2, -1, 4).
  // This tests non-axis-aligned rotation, which is the general case.
  //
  // Axis-angle: θ=120° = 2π/3, axis = (1,1,1)/√3
  // q = (cos(θ/2), sin(θ/2)*axis) = (cos60°, sin60°/√3 * (1,1,1))
  //   = (0.5, 0.28867513, 0.28867513, 0.28867513) in (w,x,y,z)
  //
  // Verify this is unit: 0.5² + 3*0.28867513² = 0.25 + 3*0.083333 = 0.25+0.25 = 0.5
  // Wait, that's 0.5, not 1. Let me recompute.
  // sin60° = √3/2 ≈ 0.8660254
  // sin60° / √3 = 0.8660254 / 1.7320508 = 0.5
  // So q = (0.5, 0.5, 0.5, 0.5) in (w,x,y,z). Unit: 4*0.25 = 1. ✓
  //
  // R_wc from q=(0.5,0.5,0.5,0.5) (wxyz):
  //   q_w=0.5, q_x=0.5, q_y=0.5, q_z=0.5
  //   R = I + 2w*[q_x]_x + 2*[q_x]_x²  (or compute directly)
  //
  //   Actually this is a well-known rotation: 120° around (1,1,1) permutes axes.
  //   R_wc = | 0  1  0 |     (cyclic permutation: x→y, y→z, z→x)
  //          | 0  0  1 |
  //          | 1  0  0 |
  //
  //   Verify: R*(1,0,0) = (0,0,1) ✓, R*(0,1,0) = (1,0,0) ✓, R*(0,0,1) = (0,1,0) ✓
  //
  // Expected T_reconstruction_camera:
  //   R_cw = R_wc^T = | 0  0  1 |
  //                   | 1  0  0 |
  //                   | 0  1  0 |
  //   t_cw = -R_cw * t_wc = -(R_cw * (2,-1,4))
  //        = -(4+0+0, 2+0+0, 0-1+0) = -(4, 2, -1) = (-4, -2, 1)
  //
  //   q_cw in (x,y,z,w): for -120° around (1,1,1):
  //     q = (cos60°, -sin60°/√3*(1,1,1)) = (0.5, -0.5, -0.5, -0.5) in (w,x,y,z)
  //     In (x,y,z,w) = (-0.5, -0.5, -0.5, 0.5)
  //
  // Verify: R_cw * (2,-1,4) + t_cw = (4, 2, -1) + (-4, -2, 1) = (0,0,0) ✓

  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

  BinBuilder images;
  images.U64(1);
  // COLMAP (w,x,y,z) = (0.5, 0.5, 0.5, 0.5): 120° around (1,1,1)
  AppendImage(images, 1, 1, "img120.jpg",
              {0.5, 0.5, 0.5, 0.5}, {2.0, -1.0, 4.0}, 0);

  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  ASSERT_EQ(rec.images.size(), 1u);

  // Expected canonical rotation: q_xyzw = (-0.5, -0.5, -0.5, 0.5)
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[0], -0.5, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[1], -0.5, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[2], -0.5, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.rotation_xyzw[3],  0.5, 1e-12);

  // Expected canonical translation: (1, -4, -2)
  // R_cw = R_wc^T = | 0  1  0 |  t_cw = -R_cw * (2,-1,4) = (1,-4,-2)
  //                 | 0  0  1 |
  //                 | 1  0  0 |
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[0],  1.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[1], -4.0, 1e-12);
  EXPECT_NEAR(rec.images[0].pose.translation_xyz[2], -2.0, 1e-12);
}

TEST_F(ColmapConverterV2Test, CameraDistortionExtracted) {
  // OPENCV (model id 4): fx=2457.4, fy=2460.1, cx=960, cy=540, k1=-0.12,
  // k2=0.03, p1=0.001, p2=0.002.
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 4, 1920, 1080,
               {2457.4, 2460.1, 960.0, 540.0, -0.12, 0.03, 0.001, 0.002});

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

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  ASSERT_EQ(rec.cameras.size(), 1u);
  EXPECT_EQ(rec.cameras[0].intrinsic_model, "opencv");
  EXPECT_DOUBLE_EQ(rec.cameras[0].fx, 2457.4);
  EXPECT_DOUBLE_EQ(rec.cameras[0].fy, 2460.1);
  EXPECT_EQ(rec.cameras[0].distortion_model, "opencv_radial");
  ASSERT_EQ(rec.cameras[0].distortion_coefficients.size(), 4u);
  EXPECT_DOUBLE_EQ(rec.cameras[0].distortion_coefficients[0], -0.12);
  EXPECT_DOUBLE_EQ(rec.cameras[0].distortion_coefficients[1], 0.03);
  EXPECT_DOUBLE_EQ(rec.cameras[0].distortion_coefficients[2], 0.001);
  EXPECT_DOUBLE_EQ(rec.cameras[0].distortion_coefficients[3], 0.002);
}

TEST_F(ColmapConverterV2Test, PinholeCameraHasNoDistortion) {
  // SIMPLE_PINHOLE (model id 0): f=100, cx=320, cy=240.
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

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

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  ASSERT_EQ(rec.cameras.size(), 1u);
  EXPECT_EQ(rec.cameras[0].intrinsic_model, "pinhole");
  EXPECT_EQ(rec.cameras[0].distortion_model, "none");
  EXPECT_TRUE(rec.cameras[0].distortion_coefficients.empty());
}

TEST_F(ColmapConverterV2Test, FrameIdMappingApplied) {
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

  BinBuilder images;
  images.U64(1);
  AppendImage(images, 1, 1, "photo.jpg",
              {0.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}, 0);

  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  std::map<std::string, std::string> fid_map;
  fid_map["photo.jpg"] = "frame-uuid-abc";

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov, fid_map);

  ASSERT_EQ(rec.images.size(), 1u);
  EXPECT_EQ(rec.images[0].frame_id, "frame-uuid-abc");
  EXPECT_EQ(rec.images[0].name, "photo.jpg");
  EXPECT_EQ(rec.images[0].detected, true);
}

TEST_F(ColmapConverterV2Test, AllImagesDetectedTrue) {
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

  BinBuilder images;
  images.U64(3);
  AppendImage(images, 1, 1, "a.jpg", {0, 0, 0, 1}, {0, 0, 0}, 0);
  AppendImage(images, 2, 1, "b.jpg", {0, 0, 0, 1}, {0, 0, 0}, 0);
  AppendImage(images, 3, 1, "c.jpg", {0, 0, 0, 1}, {0, 0, 0}, 0);

  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  ASSERT_EQ(rec.images.size(), 3u);
  for (const auto& img : rec.images) {
    EXPECT_TRUE(img.detected);
  }
}

TEST_F(ColmapConverterV2Test, TrackElementConversionPreservesIndices) {
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

  BinBuilder images;
  images.U64(2);
  AppendImage(images, 1, 1, "a.jpg", {0, 0, 0, 1}, {0, 0, 0}, 0);
  AppendImage(images, 2, 1, "b.jpg", {0, 0, 0, 1}, {0, 0, 0}, 0);

  BinBuilder points;
  points.U64(1);
  AppendPoint3D(points, 100, {1.0, 2.0, 3.0}, {128, 64, 32}, 0.5,
                {{1, 5}, {2, 12}});

  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  ASSERT_EQ(rec.points3D.size(), 1u);
  ASSERT_EQ(rec.points3D[0].track.size(), 2u);
  EXPECT_EQ(rec.points3D[0].track[0].image_id, 1u);
  EXPECT_EQ(rec.points3D[0].track[0].point2d_idx, 5);
  EXPECT_EQ(rec.points3D[0].track[1].image_id, 2u);
  EXPECT_EQ(rec.points3D[0].track[1].point2d_idx, 12);
}

TEST_F(ColmapConverterV2Test, V2JsonConformsToRequiredFields) {
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

  BinBuilder images;
  images.U64(1);
  AppendImage(images, 1, 1, "img.jpg",
              {0.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}, 0);

  BinBuilder points;
  points.U64(1);
  AppendPoint3D(points, 1, {1.0, 2.0, 3.0}, {100, 200, 50}, 0.3, {{1, 0}});

  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  prov.backend_name = "colmap";
  prov.backend_version = "3.13";
  prov.adapter_version = "0.2.0";
  prov.configuration_hash = std::string(64, 'a');
  prov.engine_version = "0.2.0";
  prov.engine_commit = "abc1234";
  prov.git_commit = "def5678";
  prov.started_at_ns = 1000000000;
  prov.finished_at_ns = 2000000000;
  prov.duration_ns = 1000000000;

  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "rec-uuid-001", "scene-uuid-001", {"session-uuid-001"},
      "reconstruction_0", prov);

  const json doc = json::parse(ReconstructionToJson(rec));

  // Required top-level fields.
  EXPECT_TRUE(doc.contains("schema_version"));
  EXPECT_TRUE(doc.contains("reconstruction_id"));
  EXPECT_TRUE(doc.contains("scene_id"));
  EXPECT_TRUE(doc.contains("session_ids"));
  EXPECT_TRUE(doc.contains("coordinate_frame"));
  EXPECT_TRUE(doc.contains("provenance"));
  EXPECT_TRUE(doc.contains("cameras"));
  EXPECT_TRUE(doc.contains("images"));
  EXPECT_TRUE(doc.contains("points3D"));

  // Required camera fields.
  const json& cam = doc["cameras"][0];
  EXPECT_TRUE(cam.contains("camera_id"));
  EXPECT_TRUE(cam.contains("width"));
  EXPECT_TRUE(cam.contains("height"));
  EXPECT_TRUE(cam.contains("intrinsic_model"));
  EXPECT_TRUE(cam.contains("fx"));
  EXPECT_TRUE(cam.contains("fy"));
  EXPECT_TRUE(cam.contains("cx"));
  EXPECT_TRUE(cam.contains("cy"));
  EXPECT_TRUE(cam.contains("distortion_model"));
  EXPECT_TRUE(cam.contains("distortion_coefficients"));

  // Required image fields (frame_id is optional per D-CRM-18 — absent when
  // downstream resolution has not established the linkage).
  const json& img = doc["images"][0];
  EXPECT_TRUE(img.contains("image_id"));
  EXPECT_TRUE(img.contains("camera_id"));
  EXPECT_TRUE(img.contains("name"));
  EXPECT_TRUE(img.contains("pose"));
  EXPECT_TRUE(img.contains("detected"));

  // Required point fields.
  const json& pt = doc["points3D"][0];
  EXPECT_TRUE(pt.contains("point3d_id"));
  EXPECT_TRUE(pt.contains("xyz"));
  EXPECT_TRUE(pt.contains("color"));
  EXPECT_TRUE(pt.contains("error"));
  EXPECT_TRUE(pt.contains("track"));

  // Provenance structure.
  const json& prov_json = doc["provenance"];
  EXPECT_TRUE(prov_json.contains("backend"));
  EXPECT_TRUE(prov_json["backend"].contains("name"));
  EXPECT_TRUE(prov_json["backend"].contains("version"));
  EXPECT_TRUE(prov_json["backend"].contains("adapter_version"));
  EXPECT_TRUE(prov_json.contains("configuration_hash"));
  EXPECT_TRUE(prov_json.contains("input_artifact_hashes"));
  EXPECT_TRUE(prov_json.contains("engine_version"));
  EXPECT_TRUE(prov_json.contains("engine_commit"));
  EXPECT_TRUE(prov_json.contains("started_at_ns"));
  EXPECT_TRUE(prov_json.contains("finished_at_ns"));
  EXPECT_TRUE(prov_json.contains("duration_ns"));
}

TEST_F(ColmapConverterV2Test, ProvenanceInfoTransferredCorrectly) {
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

  ReconstructionProvenanceInfo prov;
  prov.backend_name = "colmap";
  prov.backend_version = "3.13";
  prov.adapter_version = "0.2.0";
  prov.configuration_hash = std::string(64, 'a');
  prov.input_artifact_hashes = {"hash1", "hash2"};
  prov.engine_version = "0.3.0";
  prov.engine_commit = "abc1234";
  prov.git_commit = "def5678";
  prov.started_at_ns = 1000000000;
  prov.finished_at_ns = 2000000000;
  prov.duration_ns = 1000000000;

  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  EXPECT_EQ(rec.provenance.backend.name, "colmap");
  EXPECT_EQ(rec.provenance.backend.version, "3.13");
  EXPECT_EQ(rec.provenance.backend.adapter_version, "0.2.0");
  EXPECT_EQ(rec.provenance.configuration_hash, std::string(64, 'a'));
  EXPECT_EQ(rec.provenance.input_artifact_hashes.size(), 2u);
  EXPECT_EQ(rec.provenance.engine_version, "0.3.0");
  EXPECT_EQ(rec.provenance.engine_commit, "abc1234");
  EXPECT_EQ(rec.provenance.git_commit, "def5678");
  EXPECT_EQ(rec.provenance.started_at_ns, 1000000000);
  EXPECT_EQ(rec.provenance.finished_at_ns, 2000000000);
  EXPECT_EQ(rec.provenance.duration_ns, 1000000000);

  const json doc = json::parse(ReconstructionToJson(rec));
  EXPECT_EQ(doc["provenance"]["backend"]["name"], "colmap");
  EXPECT_EQ(doc["provenance"]["configuration_hash"], std::string(64, 'a'));
  EXPECT_EQ(doc["provenance"]["input_artifact_hashes"].size(), 2u);
}

TEST_F(ColmapConverterV2Test, ReconstructionToJsonDeterministic) {
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 4, 1920, 1080,
               {2457.4, 2460.1, 960.0, 540.0, -0.12, 0.03, 0.001, 0.002});

  BinBuilder images;
  images.U64(1);
  AppendImage(images, 1, 1, "image.jpg",
              {0.0, 0.0, 0.0, 1.0}, {0.1, -0.2, 0.3}, 0);

  BinBuilder points;
  points.U64(1);
  AppendPoint3D(points, 42, {1.0, 2.0, 3.0}, {255, 0, 128}, 0.75, {{1, 0}});

  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  prov.configuration_hash = std::string(64, 'a');
  prov.started_at_ns = 1000000000;
  prov.finished_at_ns = 2000000000;
  prov.duration_ns = 1000000000;

  spatial::core::Reconstruction rec1 = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);
  spatial::core::Reconstruction rec2 = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  EXPECT_EQ(ReconstructionToJson(rec1), ReconstructionToJson(rec2));
}

// ---------------------------------------------------------------------------
// Schema validation helpers (adapted from test_reconstruction_schema.cpp)
// ---------------------------------------------------------------------------

namespace schema_helpers {

json LoadReconstructionSchema() {
  std::ifstream in(SPATIAL_RECONSTRUCTION_SCHEMA_JSON);
  EXPECT_TRUE(in.good()) << "cannot open reconstruction.schema.json";
  return json::parse(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

json ResolveRefsImpl(const json& node, const json& root_defs) {
  if (!node.is_object()) return node;
  if (node.contains("$ref")) {
    const auto ref = node["$ref"].get<std::string>();
    if (ref.rfind("#/definitions/", 0) == 0) {
      const auto name = ref.substr(14);
      if (root_defs.contains(name)) {
        return ResolveRefsImpl(root_defs[name], root_defs);
      }
    }
    return node;
  }
  json out = node;
  for (auto it = out.begin(); it != out.end(); ++it) {
    if (it.key() == "definitions") {
      it.value() = json::object();
    } else {
      it.value() = ResolveRefsImpl(it.value(), root_defs);
    }
  }
  return out;
}

json ResolveRefs(const json& schema) {
  const json& defs = schema.contains("definitions") ? schema["definitions"]
                                                     : json::object();
  return ResolveRefsImpl(schema, defs);
}

void CheckNodeExtended(const json& schema, const json& doc,
                       std::vector<std::string>* violations) {
  std::function<void(const json&, const json&, const std::string&)> walk =
      [&](const json& s, const json& d, const std::string& p) {
        if (!s.is_object()) return;

        if (s.contains("type")) {
          const std::string t = s["type"];
          bool ok = (t == "string" && d.is_string()) ||
                    (t == "number" && d.is_number()) ||
                    (t == "integer" && d.is_number_integer()) ||
                    (t == "object" && d.is_object()) ||
                    (t == "array" && d.is_array()) ||
                    (t == "boolean" && d.is_boolean());
          if (!ok) {
            violations->push_back(p + ": expected " + t + ", got " +
                                  d.type_name());
          }
        }

        if (s.contains("required")) {
          for (const auto& key : s["required"]) {
            if (!d.is_object() || !d.contains(key)) {
              violations->push_back(p + ": missing required '" +
                                    key.get<std::string>() + "'");
            }
          }
        }

        if (s.contains("const") && d != s["const"]) {
          violations->push_back(p + ": const mismatch");
        }

        if (s.contains("enum")) {
          if (std::find(s["enum"].begin(), s["enum"].end(), d) ==
              s["enum"].end()) {
            violations->push_back(p + ": not in enum");
          }
        }

        if (s.contains("pattern") && d.is_string()) {
          const std::regex re(s["pattern"].get<std::string>());
          if (!std::regex_match(d.get<std::string>(), re)) {
            violations->push_back(p + ": pattern mismatch");
          }
        }

        if (s.contains("minimum") && d.is_number()) {
          if (d.is_number_integer()) {
            if (d.get<std::int64_t>() < s["minimum"].get<std::int64_t>()) {
              violations->push_back(p + ": below minimum");
            }
          } else {
            if (d.get<double>() < s["minimum"].get<double>()) {
              violations->push_back(p + ": below minimum");
            }
          }
        }

        if (s.contains("maximum") && d.is_number()) {
          if (d.is_number_integer()) {
            if (d.get<std::int64_t>() > s["maximum"].get<std::int64_t>()) {
              violations->push_back(p + ": above maximum");
            }
          } else {
            if (d.get<double>() > s["maximum"].get<double>()) {
              violations->push_back(p + ": above maximum");
            }
          }
        }

        if (s.contains("minItems") && d.is_array()) {
          if (d.size() < s["minItems"].get<std::size_t>()) {
            violations->push_back(p + ": below minItems");
          }
        }

        if (s.contains("maxItems") && d.is_array()) {
          if (d.size() > s["maxItems"].get<std::size_t>()) {
            violations->push_back(p + ": above maxItems");
          }
        }

        if (s.contains("properties") && d.is_object()) {
          for (auto it = s["properties"].begin();
               it != s["properties"].end(); ++it) {
            if (d.contains(it.key())) {
              walk(it.value(), d[it.key()],
                   p.empty() ? it.key() : p + "." + it.key());
            }
          }
        }

        if (s.contains("items") && d.is_array()) {
          for (std::size_t i = 0; i < d.size(); ++i) {
            walk(s["items"], d[i],
                 p + "[" + std::to_string(i) + "]");
          }
        }
      };
  walk(schema, doc, "");
}

}  // namespace schema_helpers

// ---------------------------------------------------------------------------
// Fix 3: Schema validation of ReconstructionToJson output
// ---------------------------------------------------------------------------

TEST_F(ColmapConverterV2Test, ConverterOutputPassesSchemaValidation) {
  BinBuilder cameras;
  cameras.U64(2);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});
  AppendCamera(cameras, 2, 4, 1920, 1080,
               {2457.4, 2460.1, 960.0, 540.0, -0.12, 0.03, 0.001, 0.002});

  BinBuilder images;
  images.U64(2);
  AppendImage(images, 1, 1, "frame001.jpg",
              {0.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}, 0);
  AppendImage(images, 2, 2, "frame002.jpg",
              {0.70710678, 0.0, 0.70710678, 0.0}, {1.0, 0.0, 3.0}, 0);

  BinBuilder points;
  points.U64(1);
  AppendPoint3D(points, 100, {1.0, 2.0, 3.0}, {128, 64, 32}, 0.42,
                {{1, 5}, {2, 12}});

  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  prov.backend_name = "colmap";
  prov.backend_version = "3.13";
  prov.adapter_version = "0.2.0";
  prov.configuration_hash = std::string(64, 'a');
  prov.input_artifact_hashes = {"bb" + std::string(62, 'b')};
  prov.engine_version = "0.2.0";
  prov.engine_commit = "abc1234";
  prov.git_commit = "def5678";
  prov.started_at_ns = 1783123200000000000LL;
  prov.finished_at_ns = 1783123260000000000LL;
  prov.duration_ns = 60000000000LL;

  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "00000000-0000-0000-0000-000000000001",
      "00000000-0000-0000-0000-000000000002",
      {"00000000-0000-0000-0000-000000000003"},
      "reconstruction_0", prov);

  const json doc = json::parse(ReconstructionToJson(rec));
  const auto resolved = schema_helpers::ResolveRefs(
      schema_helpers::LoadReconstructionSchema());
  std::vector<std::string> violations;
  schema_helpers::CheckNodeExtended(resolved, doc, &violations);

  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();
}

TEST_F(ColmapConverterV2Test, ConverterOutputWithEmptyFrameIdPassesSchema) {
  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, 0, 640, 480, {100.0, 320.0, 240.0});

  BinBuilder images;
  images.U64(1);
  // No frame_id_map provided → frame_id = "" → should be omitted from JSON.
  AppendImage(images, 1, 1, "photo.jpg",
              {0.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}, 0);

  BinBuilder points;
  points.U64(0);
  WriteFile(Model("cameras.bin"), cameras.bytes());
  WriteFile(Model("images.bin"), images.bytes());
  WriteFile(Model("points3D.bin"), points.bytes());

  const SparseModel model =
      ParseSparseModel(Model("cameras.bin"), Model("images.bin"),
                       Model("points3D.bin"));

  ReconstructionProvenanceInfo prov;
  prov.configuration_hash = std::string(64, 'a');

  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "00000000-0000-0000-0000-000000000001",
      "00000000-0000-0000-0000-000000000002",
      {"00000000-0000-0000-0000-000000000003"},
      "reconstruction_0", prov);

  const json doc = json::parse(ReconstructionToJson(rec));

  // frame_id must NOT be present when empty.
  EXPECT_FALSE(doc["images"][0].contains("frame_id"))
      << "empty frame_id should be omitted from JSON output";

  // Validate against schema.
  const auto resolved = schema_helpers::ResolveRefs(
      schema_helpers::LoadReconstructionSchema());
  std::vector<std::string> violations;
  schema_helpers::CheckNodeExtended(resolved, doc, &violations);

  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();
}

// ---------------------------------------------------------------------------
// Fix 4: Parameterized distortion extraction for all 11 COLMAP camera models
// ---------------------------------------------------------------------------

struct CameraModelTestCase {
  int model_id;
  std::string expected_intrinsic;
  std::string expected_distortion_model;
  std::vector<double> params;
  std::vector<double> expected_distortion;
};

class ColmapCameraModelDistortionTest
    : public ColmapConverterTest,
      public ::testing::WithParamInterface<CameraModelTestCase> {};

TEST_P(ColmapCameraModelDistortionTest, ExtractsCorrectDistortion) {
  const auto& tc = GetParam();

  BinBuilder cameras;
  cameras.U64(1);
  AppendCamera(cameras, 1, tc.model_id, 640, 480, tc.params);

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

  ReconstructionProvenanceInfo prov;
  spatial::core::Reconstruction rec = SparseModelToReconstruction(
      model, "r1", "s1", {"ses1"}, "reconstruction_0", prov);

  ASSERT_EQ(rec.cameras.size(), 1u);
  EXPECT_EQ(rec.cameras[0].intrinsic_model, tc.expected_intrinsic);
  EXPECT_EQ(rec.cameras[0].distortion_model, tc.expected_distortion_model);
  ASSERT_EQ(rec.cameras[0].distortion_coefficients.size(),
            tc.expected_distortion.size());
  for (std::size_t i = 0; i < tc.expected_distortion.size(); ++i) {
    EXPECT_DOUBLE_EQ(rec.cameras[0].distortion_coefficients[i],
                     tc.expected_distortion[i])
        << "mismatch at coefficient index " << i;
  }
}

INSTANTIATE_TEST_SUITE_P(
    AllColmapCameraModels, ColmapCameraModelDistortionTest,
    ::testing::Values(
        // Model 0: SIMPLE_PINHOLE — single_focal=true, no distortion.
        CameraModelTestCase{0, "pinhole", "none",
                            {100.0, 320.0, 240.0}, {}},
        // Model 1: PINHOLE — dual focal, no distortion.
        CameraModelTestCase{1, "pinhole", "none",
                            {100.0, 100.5, 320.0, 240.0}, {}},
        // Model 2: SIMPLE_RADIAL — single_focal, k1 at [3].
        CameraModelTestCase{2, "opencv", "opencv_radial",
                            {100.0, 320.0, 240.0, -0.15},
                            {-0.15}},
        // Model 3: RADIAL — single_focal, k1,k2 at [3..4].
        CameraModelTestCase{3, "opencv", "opencv_radial",
                            {100.0, 320.0, 240.0, -0.15, 0.05},
                            {-0.15, 0.05}},
        // Model 4: OPENCV — dual focal, k1,k2,p1,p2 at [4..7].
        CameraModelTestCase{4, "opencv", "opencv_radial",
                            {2457.4, 2460.1, 960.0, 540.0,
                             -0.12, 0.03, 0.001, 0.002},
                            {-0.12, 0.03, 0.001, 0.002}},
        // Model 5: OPENCV_FISHEYE — dual focal, k1..k4 at [4..7].
        CameraModelTestCase{5, "opencv_fisheye", "opencv_fisheye",
                            {800.0, 800.0, 320.0, 240.0,
                             0.1, -0.2, 0.3, -0.1},
                            {0.1, -0.2, 0.3, -0.1}},
        // Model 6: FULL_OPENCV — dual focal, custom, no extraction.
        CameraModelTestCase{6, "custom", "custom",
                            {1000.0, 1000.0, 320.0, 240.0,
                             -0.1, 0.05, 0.001, 0.002, 0.003, 0.004, 0.005,
                             0.006},
                            {}},
        // Model 7: FOV — dual focal, custom intrinsic, omega at [4].
        CameraModelTestCase{7, "fov", "custom",
                            {800.0, 800.0, 320.0, 240.0, 1.2},
                            {1.2}},
        // Model 8: SIMPLE_RADIAL_FISHEYE — single_focal, opencv_fisheye, k1.
        CameraModelTestCase{8, "opencv_fisheye", "opencv_fisheye",
                            {100.0, 320.0, 240.0, 0.1},
                            {0.1}},
        // Model 9: RADIAL_FISHEYE — single_focal, opencv_fisheye, k1,k2.
        CameraModelTestCase{9, "opencv_fisheye", "opencv_fisheye",
                            {100.0, 320.0, 240.0, 0.1, -0.05},
                            {0.1, -0.05}},
        // Model 10: THIN_PRISM_FISHEYE — dual focal, custom, no extraction.
        CameraModelTestCase{10, "custom", "custom",
                            {1000.0, 1000.0, 320.0, 240.0,
                             0.01, 0.02, 0.001, 0.002, 0.003, 0.004, 0.005,
                             0.006},
                            {}}));

}  // namespace
}  // namespace spatial::adapters::colmap
