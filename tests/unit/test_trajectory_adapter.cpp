#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adapters/colmap/colmap_converter.h"
#include "adapters/colmap/colmap_trajectory_adapter.h"
#include "core/errors/project_error.h"
#include "core/trajectory/trajectory.h"
#include "tests/unit/schema_check.h"

namespace spatial::adapters::colmap {
namespace {

// Env lookup that avoids the MSVC C4996 deprecation warning for getenv.
const char* GetEnv(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  size_t len = 0;
  if (_dupenv_s(&value, &len, name) == 0 && value != nullptr) {
    static thread_local std::string storage;
    storage = value;
    free(value);
    return storage.c_str();
  }
  return nullptr;
#else
  return std::getenv(name);
#endif
}

// Test fixture with standard metadata.
class ColmapTrajectoryAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    trajectory_id_ = "550e8400-e29b-41d4-a716-446655440000";
    scene_id_ = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
    session_id_ = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
    coordinate_frame_ = "trajectory_0";

    prov_.backend_name = "colmap";
    prov_.backend_version = "3.8";
    prov_.adapter_version = "0.1.0";
    prov_.configuration_hash = "abc123";
    prov_.engine_version = "1.0.0";
  }

  // Helper: build a SparseModel with a single image.
  SparseModel MakeSingleImage(
      const std::array<double, 4>& qvec_wxyz,
      const std::array<double, 3>& tvec,
      const std::string& name = "IMG_0001.jpg",
      std::uint32_t image_id = 1,
      std::uint32_t camera_id = 1) {
    SparseModel model;
    SparseModelCamera cam;
    cam.camera_id = camera_id;
    cam.model = "PINHOLE";
    cam.intrinsic_model = "pinhole";
    cam.model_id = 1;
    cam.width = 640;
    cam.height = 480;
    cam.intrinsics = {500.0, 500.0, 320.0, 240.0};
    cam.raw_params = {500.0, 500.0, 320.0, 240.0};
    model.cameras.push_back(cam);

    SparseModelImage img;
    img.image_id = image_id;
    img.camera_id = camera_id;
    img.name = name;
    img.qvec = qvec_wxyz;
    img.tvec = tvec;
    model.images.push_back(img);

    return model;
  }

  std::string trajectory_id_;
  std::string scene_id_;
  std::string session_id_;
  std::string coordinate_frame_;
  TrajectoryProvenanceInfo prov_;
};

// =========================================================================
// Pose Transform Direction Tests
// =========================================================================
// These tests prove that the adapter correctly converts
// COLMAP camera-to-world → T_trajectory_camera (world-from-camera).
// Each test specifies the COLMAP input, the expected T_trajectory_camera
// output, and the mathematical reasoning.

TEST_F(ColmapTrajectoryAdapterTest, IdentityPose) {
  // COLMAP: qvec=(1,0,0,0) = identity (w=1), tvec=(0,0,0)
  // Camera is at the origin, looking along +Z (COLMAP convention).
  // T_trajectory_camera should be identity: camera maps to origin with
  // no rotation.
  // Transform proof: Invert(identity, 0) = (identity, 0).
  auto model = MakeSingleImage({1.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 1u);
  const auto& node = result.nodes[0];

  EXPECT_DOUBLE_EQ(node.position_xyz[0], 0.0);
  EXPECT_DOUBLE_EQ(node.position_xyz[1], 0.0);
  EXPECT_DOUBLE_EQ(node.position_xyz[2], 0.0);
  // Quaternion: (x, y, z, w) = (0, 0, 0, 1) = identity
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[0], 0.0);
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[1], 0.0);
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[2], 0.0);
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[3], 1.0);
}

TEST_F(ColmapTrajectoryAdapterTest, PureTranslation) {
  // COLMAP: qvec=(1,0,0,0) = identity, tvec=(5,3,1)
  // Camera-to-world: camera at origin, world origin at (-5,-3,-1) relative
  // to camera. Inverting: T_trajectory_camera.position = -R^T * t = -I * t
  // = (-5, -3, -1).
  // Transform proof: R_cw = I, t_cw = -I * (5,3,1) = (-5,-3,-1).
  auto model = MakeSingleImage({1.0, 0.0, 0.0, 0.0}, {5.0, 3.0, 1.0});
  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 1u);
  const auto& node = result.nodes[0];

  EXPECT_DOUBLE_EQ(node.position_xyz[0], -5.0);
  EXPECT_DOUBLE_EQ(node.position_xyz[1], -3.0);
  EXPECT_DOUBLE_EQ(node.position_xyz[2], -1.0);
  // Rotation: identity (no rotation, only translation inversion)
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[0], 0.0);
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[1], 0.0);
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[2], 0.0);
  EXPECT_DOUBLE_EQ(node.rotation_xyzw[3], 1.0);
}

TEST_F(ColmapTrajectoryAdapterTest, Rotation90DegreesAroundZ) {
  // COLMAP: 90° rotation around Z-axis (camera looking right).
  // qvec_wxyz = (cos45, 0, 0, sin45) = (0.7071, 0, 0, 0.7071)
  // tvec = (2, 0, 0)
  //
  // COLMAP T_camworld = R_wc * p + t_wc where:
  //   R_wc = 90° about Z = [[0,-1,0],[1,0,0],[0,0,1]]
  //   t_wc = (2, 0, 0)
  //
  // Inverting for T_trajectory_camera:
  //   R_cw = R_wc^T = [[0,1,0],[-1,0,0],[0,0,1]]
  //   t_cw = -R_wc^T * t_wc = -[[0,1,0],[-1,0,0],[0,0,1]] * (2,0,0)
  //        = -(0, -2, 0) = (0, 2, 0)
  //
  // Quaternion (x,y,z,w) for R_cw:
  //   90° about Z (inverted direction): (0, 0, -sin45, cos45) = (0, 0, -0.7071, 0.7071)
  //
  // Note: qvec (w,x,y,z) = (0.7071, 0, 0, 0.7071) represents 90° about Z.
  // Conjugate = (0, 0, 0, 0.7071) - wait, let me recalculate.
  // Actually q_xyzw = (0, 0, 0.7071, 0.7071) after reorder.
  // Conjugate: (-0, -0, -0.7071, 0.7071) = (0, 0, -0.7071, 0.7071).
  const double s = std::sqrt(2.0) / 2.0;  // ~0.7071

  auto model = MakeSingleImage(
      {s, 0.0, 0.0, s},  // qvec_wxyz = (cos45, 0, 0, sin45)
      {2.0, 0.0, 0.0});  // tvec = (2, 0, 0)

  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 1u);
  const auto& node = result.nodes[0];

  // Position: t_cw = (0, 2, 0)
  EXPECT_NEAR(node.position_xyz[0], 0.0, 1e-10);
  EXPECT_NEAR(node.position_xyz[1], 2.0, 1e-10);
  EXPECT_NEAR(node.position_xyz[2], 0.0, 1e-10);

  // Rotation: R_cw quaternion (x,y,z,w) = (0, 0, -sin45, cos45)
  EXPECT_NEAR(node.rotation_xyzw[0], 0.0, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[1], 0.0, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[2], -s, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[3], s, 1e-10);
}

TEST_F(ColmapTrajectoryAdapterTest, Rotation180DegreesAroundZ) {
  // COLMAP: 180° rotation around Z-axis.
  // qvec_wxyz = (0, 0, 0, 1) → q_xyzw = (0, 0, 0, 1)
  // tvec = (1, -2, 3)
  //
  // COLMAP T_camworld: R_wc = 180° about Z = [[−1,0,0],[0,−1,0],[0,0,1]]
  // t_wc = (1, -2, 3)
  //
  // Inverting:
  //   R_cw = R_wc^T = R_wc (180° rotation is symmetric)
  //   t_cw = -R_wc^T * t_wc = -[[-1,0,0],[0,-1,0],[0,0,1]] * (1,-2,3)
  //        = -(-1, 2, 3) = (1, -2, -3)
  //
  // Quaternion (x,y,z,w) for 180° about Z: (0, 0, 1, 0)
  auto model = MakeSingleImage(
      {0.0, 0.0, 0.0, 1.0},  // qvec_wxyz = identity*2 → 180° about Z
      {1.0, -2.0, 3.0});

  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 1u);
  const auto& node = result.nodes[0];

  // Position: (1, -2, -3)
  EXPECT_NEAR(node.position_xyz[0], 1.0, 1e-10);
  EXPECT_NEAR(node.position_xyz[1], -2.0, 1e-10);
  EXPECT_NEAR(node.position_xyz[2], -3.0, 1e-10);

  // Rotation: (x,y,z,w) = (0, 0, 1, 0)
  EXPECT_NEAR(node.rotation_xyzw[0], 0.0, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[1], 0.0, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[2], 1.0, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[3], 0.0, 1e-10);
}

TEST_F(ColmapTrajectoryAdapterTest, ArbitraryRotation) {
  // COLMAP: 90° rotation around X-axis + translation.
  // qvec_wxyz = (cos45, sin45, 0, 0) = (s, s, 0, 0) where s = sqrt(2)/2
  // q_xyzw = (s, 0, 0, s) after reorder → 90° about X
  // tvec = (1, 2, 3)
  //
  // COLMAP T_camworld: R_wc = 90° about X = [[1,0,0],[0,0,-1],[0,1,0]]
  // t_wc = (1, 2, 3)
  //
  // Inverting:
  //   R_cw = R_wc^T = [[1,0,0],[0,0,1],[0,-1,0]]
  //   t_cw = -R_wc^T * t_wc = -[[1,0,0],[0,0,1],[0,-1,0]] * (1,2,3)
  //        = -(1, 3, -2) = (-1, -3, 2)
  //
  // Quaternion for R_cw (90° about -X): (x,y,z,w) = (-s, 0, 0, s)
  const double s = std::sqrt(2.0) / 2.0;

  auto model = MakeSingleImage(
      {s, s, 0.0, 0.0},  // qvec_wxyz = (cos45, sin45, 0, 0) → 90° about X
      {1.0, 2.0, 3.0});

  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 1u);
  const auto& node = result.nodes[0];

  // Position: (-1, -3, 2)
  EXPECT_NEAR(node.position_xyz[0], -1.0, 1e-10);
  EXPECT_NEAR(node.position_xyz[1], -3.0, 1e-10);
  EXPECT_NEAR(node.position_xyz[2], 2.0, 1e-10);

  // Rotation: (x,y,z,w) = (-sin45, 0, 0, cos45)
  EXPECT_NEAR(node.rotation_xyzw[0], -s, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[1], 0.0, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[2], 0.0, 1e-10);
  EXPECT_NEAR(node.rotation_xyzw[3], s, 1e-10);
}

// =========================================================================
// Multi-sample / Sequence Tests
// =========================================================================

TEST_F(ColmapTrajectoryAdapterTest, MultipleImagesSequenceAndDistance) {
  // 3 images: identity at origin, identity at (1,0,0), identity at (1,1,0).
  // Sequence index should be 0, 1, 2 (sorted by image_id).
  // Total distance = 1.0 + 1.0 = 2.0.
  // All rotations: identity (no rotation in COLMAP input).
  // Positions: (-0,-0,-0), (-1,0,0), (-1,-1,0) after inversion.
  SparseModel model;

  SparseModelCamera cam;
  cam.camera_id = 1;
  cam.model = "PINHOLE";
  cam.intrinsic_model = "pinhole";
  cam.model_id = 1;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsics = {500.0, 500.0, 320.0, 240.0};
  cam.raw_params = {500.0, 500.0, 320.0, 240.0};
  model.cameras.push_back(cam);

  // Image 1: identity at origin
  {
    SparseModelImage img;
    img.image_id = 10;
    img.camera_id = 1;
    img.name = "img_010.jpg";
    img.qvec = {1.0, 0.0, 0.0, 0.0};
    img.tvec = {0.0, 0.0, 0.0};
    model.images.push_back(img);
  }
  // Image 2: identity, translated
  {
    SparseModelImage img;
    img.image_id = 20;
    img.camera_id = 1;
    img.name = "img_020.jpg";
    img.qvec = {1.0, 0.0, 0.0, 0.0};
    img.tvec = {-1.0, 0.0, 0.0};  // after inversion: position (1, 0, 0)
    model.images.push_back(img);
  }
  // Image 3: identity, translated
  {
    SparseModelImage img;
    img.image_id = 30;
    img.camera_id = 1;
    img.name = "img_030.jpg";
    img.qvec = {1.0, 0.0, 0.0, 0.0};
    img.tvec = {-1.0, -1.0, 0.0};  // after inversion: position (1, 1, 0)
    model.images.push_back(img);
  }

  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 3u);

  // Sequence indices: 0, 1, 2
  EXPECT_EQ(result.nodes[0].sequence_index, 0);
  EXPECT_EQ(result.nodes[1].sequence_index, 1);
  EXPECT_EQ(result.nodes[2].sequence_index, 2);

  // Positions (after inversion of COLMAP tvec)
  EXPECT_NEAR(result.nodes[0].position_xyz[0], 0.0, 1e-10);
  EXPECT_NEAR(result.nodes[0].position_xyz[1], 0.0, 1e-10);
  EXPECT_NEAR(result.nodes[0].position_xyz[2], 0.0, 1e-10);

  EXPECT_NEAR(result.nodes[1].position_xyz[0], 1.0, 1e-10);
  EXPECT_NEAR(result.nodes[1].position_xyz[1], 0.0, 1e-10);
  EXPECT_NEAR(result.nodes[1].position_xyz[2], 0.0, 1e-10);

  EXPECT_NEAR(result.nodes[2].position_xyz[0], 1.0, 1e-10);
  EXPECT_NEAR(result.nodes[2].position_xyz[1], 1.0, 1e-10);
  EXPECT_NEAR(result.nodes[2].position_xyz[2], 0.0, 1e-10);

  // Total distance: 1.0 + 1.0 = 2.0
  EXPECT_DOUBLE_EQ(result.trajectory.total_distance_m, 2.0);
  EXPECT_EQ(result.trajectory.node_count, 3);
}

TEST_F(ColmapTrajectoryAdapterTest, ImageIdOrderingDeterminesSequence) {
  // Images arrive in the model with non-sequential IDs.
  // After SortImageById in ParseSparseModel, the order is by image_id.
  // Our adapter processes them in model.images order (already sorted).
  SparseModel model;

  SparseModelCamera cam;
  cam.camera_id = 1;
  cam.model = "PINHOLE";
  cam.intrinsic_model = "pinhole";
  cam.model_id = 1;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsics = {500.0, 500.0, 320.0, 240.0};
  cam.raw_params = {500.0, 500.0, 320.0, 240.0};
  model.cameras.push_back(cam);

  // Insert out of order — adapter processes in vector order.
  // (In real code, ParseSparseModel sorts by image_id first.)
  for (auto [id, name] : std::vector<std::pair<std::uint32_t, std::string>>{
           {3, "c.jpg"}, {1, "a.jpg"}, {2, "b.jpg"}}) {
    SparseModelImage img;
    img.image_id = id;
    img.camera_id = 1;
    img.name = name;
    img.qvec = {1.0, 0.0, 0.0, 0.0};
    img.tvec = {0.0, 0.0, 0.0};
    model.images.push_back(img);
  }

  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 3u);

  // Sequence index is assigned in vector order (0, 1, 2).
  EXPECT_EQ(result.nodes[0].sequence_index, 0);
  EXPECT_EQ(result.nodes[1].sequence_index, 1);
  EXPECT_EQ(result.nodes[2].sequence_index, 2);

  // Names follow vector order
  EXPECT_EQ(result.nodes[0].frame_id, "");  // no frame_id_map
  EXPECT_EQ(result.nodes[1].frame_id, "");
  EXPECT_EQ(result.nodes[2].frame_id, "");
}

// =========================================================================
// Frame ID Mapping Tests
// =========================================================================

TEST_F(ColmapTrajectoryAdapterTest, FrameIdMappingApplied) {
  auto model = MakeSingleImage({1.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
                               "IMG_0001.jpg");

  std::map<std::string, std::string> frame_id_map;
  frame_id_map["IMG_0001.jpg"] = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";

  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_, frame_id_map);

  ASSERT_EQ(result.nodes.size(), 1u);
  EXPECT_EQ(result.nodes[0].frame_id,
            "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
}

TEST_F(ColmapTrajectoryAdapterTest, MissingFrameIdMappingEmpty) {
  auto model = MakeSingleImage({1.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0},
                               "IMG_0001.jpg");

  // Empty frame_id_map → frame_id remains empty
  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 1u);
  EXPECT_TRUE(result.nodes[0].frame_id.empty());
}

TEST_F(ColmapTrajectoryAdapterTest, PartialFrameIdMapping) {
  SparseModel model;

  SparseModelCamera cam;
  cam.camera_id = 1;
  cam.model = "PINHOLE";
  cam.intrinsic_model = "pinhole";
  cam.model_id = 1;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsics = {500.0, 500.0, 320.0, 240.0};
  cam.raw_params = {500.0, 500.0, 320.0, 240.0};
  model.cameras.push_back(cam);

  {
    SparseModelImage img;
    img.image_id = 1;
    img.camera_id = 1;
    img.name = "mapped.jpg";
    img.qvec = {1.0, 0.0, 0.0, 0.0};
    img.tvec = {0.0, 0.0, 0.0};
    model.images.push_back(img);
  }
  {
    SparseModelImage img;
    img.image_id = 2;
    img.camera_id = 1;
    img.name = "unmapped.jpg";
    img.qvec = {1.0, 0.0, 0.0, 0.0};
    img.tvec = {0.0, 0.0, 0.0};
    model.images.push_back(img);
  }

  std::map<std::string, std::string> frame_id_map;
  frame_id_map["mapped.jpg"] = "11111111-2222-3333-4444-555555555555";

  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_, frame_id_map);

  ASSERT_EQ(result.nodes.size(), 2u);
  EXPECT_EQ(result.nodes[0].frame_id,
            "11111111-2222-3333-4444-555555555555");
  EXPECT_TRUE(result.nodes[1].frame_id.empty());
}

// =========================================================================
// Timestamp Semantics Tests
// =========================================================================

TEST_F(ColmapTrajectoryAdapterTest, TimestampNsIsZero) {
  // COLMAP images.bin does not carry timestamps.
  // The adapter must NOT fabricate timestamps.
  auto model = MakeSingleImage({1.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  ASSERT_EQ(result.nodes.size(), 1u);
  EXPECT_EQ(result.nodes[0].timestamp_ns, 0);
}

// =========================================================================
// Trajectory Metadata Tests
// =========================================================================

TEST_F(ColmapTrajectoryAdapterTest, TrajectoryMetadata) {
  auto model = MakeSingleImage({1.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
  auto result = SparseModelToTrajectory(
      model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_);

  const auto& traj = result.trajectory;
  EXPECT_EQ(traj.trajectory_id, trajectory_id_);
  EXPECT_EQ(traj.scene_id, scene_id_);
  EXPECT_EQ(traj.session_id, session_id_);
  EXPECT_EQ(traj.kind, "sfm");
  EXPECT_EQ(traj.coordinate_frame, coordinate_frame_);
  EXPECT_EQ(traj.status, "building");
  EXPECT_EQ(traj.node_count, 1);

  // Provenance
  EXPECT_EQ(traj.provenance.backend.name, "colmap");
  EXPECT_EQ(traj.provenance.backend.version, "3.8");
  EXPECT_EQ(traj.provenance.backend.adapter_version, "0.1.0");
}

TEST_F(ColmapTrajectoryAdapterTest, EmptyModelThrows) {
  SparseModel empty_model;
  // No images → should throw
  EXPECT_THROW(SparseModelToTrajectory(
      empty_model, trajectory_id_, scene_id_, session_id_,
      coordinate_frame_, prov_),
               spatial::core::AdapterError);
}

// =========================================================================
// Schema Validation Tests
// =========================================================================

class TrajectorySchemaValidationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* schema_path = GetEnv("SPATIAL_TRAJECTORY_SCHEMA_JSON");
    ASSERT_NE(schema_path, nullptr)
        << "SPATIAL_TRAJECTORY_SCHEMA_JSON not set";
    std::ifstream ifs(schema_path);
    ASSERT_TRUE(ifs.is_open()) << "Cannot open " << schema_path;
    schema_ = nlohmann::json::parse(ifs);
  }

  nlohmann::json schema_;
};

nlohmann::json TrajectoryToJson(const core::TrajectoryExtractionResult& res) {
  nlohmann::json doc;
  doc["schema_version"] = 1;
  doc["trajectory_id"] = res.trajectory.trajectory_id;

  nlohmann::json nodes = nlohmann::json::array();
  for (const auto& node : res.nodes) {
    nlohmann::json n;
    n["frame_id"] = node.frame_id;
    n["timestamp_ns"] = node.timestamp_ns;
    n["sequence_index"] = node.sequence_index;
    n["position_xyz"] = {node.position_xyz[0], node.position_xyz[1],
                         node.position_xyz[2]};
    n["rotation_xyzw"] = {node.rotation_xyzw[0], node.rotation_xyzw[1],
                          node.rotation_xyzw[2], node.rotation_xyzw[3]};
    nodes.push_back(std::move(n));
  }
  doc["nodes"] = std::move(nodes);
  return doc;
}

TEST_F(TrajectorySchemaValidationTest, ValidDocument) {
  // Build a valid trajectory result with frame_ids populated.
  SparseModel model;

  SparseModelCamera cam;
  cam.camera_id = 1;
  cam.model = "PINHOLE";
  cam.intrinsic_model = "pinhole";
  cam.model_id = 1;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsics = {500.0, 500.0, 320.0, 240.0};
  cam.raw_params = {500.0, 500.0, 320.0, 240.0};
  model.cameras.push_back(cam);

  {
    SparseModelImage img;
    img.image_id = 1;
    img.camera_id = 1;
    img.name = "img1.jpg";
    img.qvec = {1.0, 0.0, 0.0, 0.0};
    img.tvec = {0.0, 0.0, 0.0};
    model.images.push_back(img);
  }
  {
    SparseModelImage img;
    img.image_id = 2;
    img.camera_id = 1;
    img.name = "img2.jpg";
    img.qvec = {1.0, 0.0, 0.0, 0.0};
    img.tvec = {-1.0, 0.0, 0.0};
    model.images.push_back(img);
  }

  std::map<std::string, std::string> frame_id_map;
  frame_id_map["img1.jpg"] = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
  frame_id_map["img2.jpg"] = "ffffffff-1111-2222-3333-444444444444";

  TrajectoryProvenanceInfo prov;
  prov.backend_name = "colmap";
  prov.backend_version = "3.8";

  auto result = SparseModelToTrajectory(
      model,
      "11111111-2222-3333-4444-555555555555",
      "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
      "bbbbbbbb-cccc-dddd-eeee-ffffffffffff",
      "trajectory_0", prov, frame_id_map);

  nlohmann::json doc = TrajectoryToJson(result);

  std::vector<std::string> violations;
  CheckNode(schema_, doc, "$", &violations);
  EXPECT_TRUE(violations.empty())
      << "Schema violations: "
      << [&]() {
           std::string s;
           for (const auto& v : violations) s += "\n  " + v;
           return s;
         }();
}

TEST_F(TrajectorySchemaValidationTest, MissingFrameIdStillValid) {
  // Schema requires frame_id as string; empty string is still a string.
  SparseModel model;

  SparseModelCamera cam;
  cam.camera_id = 1;
  cam.model = "PINHOLE";
  cam.intrinsic_model = "pinhole";
  cam.model_id = 1;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsics = {500.0, 500.0, 320.0, 240.0};
  cam.raw_params = {500.0, 500.0, 320.0, 240.0};
  model.cameras.push_back(cam);

  SparseModelImage img;
  img.image_id = 1;
  img.camera_id = 1;
  img.name = "img1.jpg";
  img.qvec = {1.0, 0.0, 0.0, 0.0};
  img.tvec = {0.0, 0.0, 0.0};
  model.images.push_back(img);

  TrajectoryProvenanceInfo prov;
  prov.backend_name = "colmap";

  auto result = SparseModelToTrajectory(
      model,
      "11111111-2222-3333-4444-555555555555",
      "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
      "bbbbbbbb-cccc-dddd-eeee-ffffffffffff",
      "trajectory_0", prov);

  nlohmann::json doc = TrajectoryToJson(result);

  std::vector<std::string> violations;
  CheckNode(schema_, doc, "$", &violations);
  EXPECT_TRUE(violations.empty())
      << "Schema violations: "
      << [&]() {
           std::string s;
           for (const auto& v : violations) s += "\n  " + v;
           return s;
         }();
}

}  // namespace
}  // namespace spatial::adapters::colmap
