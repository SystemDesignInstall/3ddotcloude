#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include "core/reconstruction/reconstruction.h"
#include "core/errors/project_error.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"

namespace spatial::core {
namespace {

// --- Domain type tests ---

TEST(ReconPose, DefaultConstruction) {
  ReconPose pose;
  // Default-constructed array is all zeros (value-initialized).
  EXPECT_DOUBLE_EQ(pose.rotation_xyzw[0], 0.0);
  EXPECT_DOUBLE_EQ(pose.rotation_xyzw[1], 0.0);
  EXPECT_DOUBLE_EQ(pose.rotation_xyzw[2], 0.0);
  EXPECT_DOUBLE_EQ(pose.rotation_xyzw[3], 0.0);
  EXPECT_DOUBLE_EQ(pose.translation_xyz[0], 0.0);
  EXPECT_DOUBLE_EQ(pose.translation_xyz[1], 0.0);
  EXPECT_DOUBLE_EQ(pose.translation_xyz[2], 0.0);
}

TEST(ReconPose, Equality) {
  ReconPose a;
  a.rotation_xyzw = {0.1, 0.2, 0.3, 0.9};
  a.translation_xyz = {1.0, 2.0, 3.0};

  ReconPose b = a;
  EXPECT_EQ(a, b);

  ReconPose c = a;
  c.translation_xyz[0] = 99.0;
  EXPECT_NE(a, c);
}

TEST(ReconCamera, DefaultConstruction) {
  ReconCamera cam;
  EXPECT_EQ(cam.camera_id, 0u);
  EXPECT_EQ(cam.width, 0);
  EXPECT_EQ(cam.height, 0);
  EXPECT_TRUE(cam.intrinsic_model.empty());
  EXPECT_DOUBLE_EQ(cam.fx, 0.0);
  EXPECT_TRUE(cam.distortion_model.empty());
  EXPECT_TRUE(cam.distortion_coefficients.empty());
  EXPECT_TRUE(cam.calibration_ref.empty());
}

TEST(ReconCamera, Equality) {
  ReconCamera a;
  a.camera_id = 1;
  a.width = 1920;
  a.height = 1080;
  a.intrinsic_model = "pinhole";
  a.fx = 1000.0;
  a.fy = 1000.0;
  a.cx = 960.0;
  a.cy = 540.0;
  a.distortion_model = "none";
  a.calibration_ref = "550e8400-e29b-41d4-a716-446655440000";

  ReconCamera b = a;
  EXPECT_EQ(a, b);

  ReconCamera c = a;
  c.camera_id = 2;
  EXPECT_NE(a, c);
}

TEST(ReconImage, DefaultConstruction) {
  ReconImage img;
  EXPECT_EQ(img.image_id, 0u);
  EXPECT_EQ(img.camera_id, 0u);
  EXPECT_TRUE(img.frame_id.empty());
  EXPECT_TRUE(img.name.empty());
  EXPECT_EQ(img.pose, ReconPose{});
  EXPECT_FALSE(img.detected);
}

TEST(ReconImage, Equality) {
  ReconImage a;
  a.image_id = 10;
  a.camera_id = 1;
  a.frame_id = "550e8400-e29b-41d4-a716-446655440000";
  a.name = "IMG_0001.jpg";
  a.detected = true;

  ReconImage b = a;
  EXPECT_EQ(a, b);

  ReconImage c = a;
  c.detected = false;
  EXPECT_NE(a, c);
}

TEST(ReconPoint3D, DefaultConstruction) {
  ReconPoint3D pt;
  EXPECT_EQ(pt.point3d_id, 0u);
  EXPECT_DOUBLE_EQ(pt.xyz[0], 0.0);
  EXPECT_DOUBLE_EQ(pt.error, 0.0);
  EXPECT_TRUE(pt.track.empty());
}

TEST(ReconPoint3D, TrackElements) {
  ReconPoint3D pt;
  pt.point3d_id = 42;
  pt.track.push_back({.image_id = 1, .point2d_idx = 5});
  pt.track.push_back({.image_id = 3, .point2d_idx = 12});

  EXPECT_EQ(pt.track.size(), 2u);
  EXPECT_EQ(pt.track[0].image_id, 1u);
  EXPECT_EQ(pt.track[0].point2d_idx, 5);
  EXPECT_EQ(pt.track[1].image_id, 3u);
  EXPECT_EQ(pt.track[1].point2d_idx, 12);
}

TEST(ReconPoint3D, Equality) {
  ReconPoint3D a;
  a.point3d_id = 1;
  a.xyz = {1.0, 2.0, 3.0};
  a.track.push_back({.image_id = 1, .point2d_idx = 0});

  ReconPoint3D b = a;
  EXPECT_EQ(a, b);

  ReconPoint3D c = a;
  c.track.push_back({.image_id = 2, .point2d_idx = 1});
  EXPECT_NE(a, c);
}

TEST(ReconObservation, Equality) {
  ReconObservation a{.image_id = 1, .point2d_idx = 5};
  ReconObservation b = a;
  EXPECT_EQ(a, b);

  ReconObservation c{.image_id = 1, .point2d_idx = 6};
  EXPECT_NE(a, c);
}

TEST(ReconstructionProvenance, DefaultConstruction) {
  ReconstructionProvenance prov;
  EXPECT_TRUE(prov.backend.name.empty());
  EXPECT_TRUE(prov.backend.version.empty());
  EXPECT_TRUE(prov.configuration_hash.empty());
  EXPECT_TRUE(prov.input_artifact_hashes.empty());
  EXPECT_EQ(prov.started_at_ns, 0);
  EXPECT_EQ(prov.finished_at_ns, 0);
}

TEST(ReconstructionProvenance, Equality) {
  ReconstructionProvenance a;
  a.backend.name = "colmap";
  a.backend.version = "3.13";
  a.started_at_ns = 1000;
  a.finished_at_ns = 2000;

  ReconstructionProvenance b = a;
  EXPECT_EQ(a, b);

  ReconstructionProvenance c = a;
  c.backend.version = "3.14";
  EXPECT_NE(a, c);
}

TEST(Reconstruction, DefaultConstruction) {
  Reconstruction rec;
  EXPECT_TRUE(rec.reconstruction_id.empty());
  EXPECT_TRUE(rec.scene_id.empty());
  EXPECT_TRUE(rec.session_ids.empty());
  EXPECT_TRUE(rec.coordinate_frame.empty());
  EXPECT_TRUE(rec.status.empty());
  EXPECT_EQ(rec.created_at_ns, 0);
  EXPECT_TRUE(rec.cameras.empty());
  EXPECT_TRUE(rec.images.empty());
  EXPECT_TRUE(rec.points3D.empty());
}

TEST(Reconstruction, FullConstruction) {
  Reconstruction rec;
  rec.reconstruction_id = "550e8400-e29b-41d4-a716-446655440000";
  rec.scene_id = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
  rec.session_ids.push_back("6ba7b811-9dad-11d1-80b4-00c04fd430c8");
  rec.coordinate_frame = "reconstruction_0";
  rec.status = "succeeded";
  rec.created_at_ns = 1234567890;

  rec.provenance.backend.name = "colmap";
  rec.provenance.backend.version = "3.13";

  ReconCamera cam;
  cam.camera_id = 1;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsic_model = "pinhole";
  cam.fx = 500.0;
  cam.fy = 500.0;
  cam.cx = 320.0;
  cam.cy = 240.0;
  cam.distortion_model = "none";
  rec.cameras.push_back(cam);

  ReconImage img;
  img.image_id = 1;
  img.camera_id = 1;
  img.frame_id = "6ba7b812-9dad-11d1-80b4-00c04fd430c8";
  img.name = "frame001.jpg";
  img.detected = true;
  img.pose.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  img.pose.translation_xyz = {0.0, 0.0, 0.0};
  rec.images.push_back(img);

  ReconPoint3D pt;
  pt.point3d_id = 100;
  pt.xyz = {1.0, 2.0, 3.0};
  pt.color = {255, 128, 0};
  pt.error = 0.5;
  pt.track.push_back({.image_id = 1, .point2d_idx = 0});
  rec.points3D.push_back(pt);

  EXPECT_EQ(rec.cameras.size(), 1u);
  EXPECT_EQ(rec.images.size(), 1u);
  EXPECT_EQ(rec.points3D.size(), 1u);
  EXPECT_EQ(rec.status, "succeeded");
  EXPECT_EQ(rec.provenance.backend.name, "colmap");
}

TEST(Reconstruction, Equality) {
  Reconstruction a;
  a.reconstruction_id = "550e8400-e29b-41d4-a716-446655440000";
  a.status = "succeeded";

  Reconstruction b = a;
  EXPECT_EQ(a, b);

  Reconstruction c = a;
  c.status = "failed";
  EXPECT_NE(a, c);
}

// --- Database tests ---

class ReconstructionDbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_recon_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    path_ = root_ / "project.db";
    db_ = MetadataDb::Create(path_);
    // Create a project and scene (required by FK constraints on reconstructions).
    project_id_ = GenerateUuid();
    db_.InsertProject(project_id_, "test_project", 1, "{}", 1000, "ENU", "world",
                      "{}", "{}");
    scene_ = db_.FindOrCreateScene(project_id_, "test_scene", "{}", 2000);
  }

  void TearDown() override {
    db_.Close();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
  std::filesystem::path path_;
  MetadataDb db_;
  Uuid project_id_{};
  SceneRow scene_{};
};

TEST_F(ReconstructionDbTest, Migration0007Applied) {
  // If migration 0007 fails, the fixture SetUp() will throw. Verify the table
  // accepts inserts.
  ReconstructionRow row;
  row.reconstruction_id = GenerateUuid();
  row.scene_id = scene_.scene_id;
  row.coordinate_frame = "reconstruction_0";
  row.status = "succeeded";
  row.created_at_ns = 1000;
  row.document_json = "{}";
  EXPECT_NO_THROW(db_.AddReconstruction(row));
}

TEST_F(ReconstructionDbTest, InsertAndQueryLatest) {
  // Insert two reconstructions for the same scene.
  ReconstructionRow r1;
  r1.reconstruction_id = GenerateUuid();
  r1.scene_id = scene_.scene_id;
  r1.coordinate_frame = "reconstruction_0";
  r1.status = "succeeded";
  r1.created_at_ns = 1000;
  r1.document_json = R"({"reconstruction_id":"r1"})";
  db_.AddReconstruction(r1);

  ReconstructionRow r2;
  r2.reconstruction_id = GenerateUuid();
  r2.scene_id = scene_.scene_id;
  r2.coordinate_frame = "reconstruction_0";
  r2.status = "succeeded";
  r2.created_at_ns = 2000;
  r2.document_json = R"({"reconstruction_id":"r2"})";
  db_.AddReconstruction(r2);

  // QueryLatestReconstructionByScene returns the most recent succeeded one.
  const auto latest = db_.QueryLatestReconstructionByScene(scene_.scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, r2.reconstruction_id);
  EXPECT_EQ(latest->created_at_ns, 2000);
  EXPECT_EQ(latest->status, "succeeded");
}

TEST_F(ReconstructionDbTest, QueryLatestSkipsSuperseded) {
  ReconstructionRow r1;
  r1.reconstruction_id = GenerateUuid();
  r1.scene_id = scene_.scene_id;
  r1.coordinate_frame = "reconstruction_0";
  r1.status = "superseded";
  r1.created_at_ns = 1000;
  r1.document_json = "{}";
  db_.AddReconstruction(r1);

  ReconstructionRow r2;
  r2.reconstruction_id = GenerateUuid();
  r2.scene_id = scene_.scene_id;
  r2.coordinate_frame = "reconstruction_0";
  r2.status = "succeeded";
  r2.created_at_ns = 2000;
  r2.document_json = "{}";
  db_.AddReconstruction(r2);

  const auto latest = db_.QueryLatestReconstructionByScene(scene_.scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, r2.reconstruction_id);
}

TEST_F(ReconstructionDbTest, QueryLatestReturnsEmptyWhenNoSucceeded) {
  ReconstructionRow r1;
  r1.reconstruction_id = GenerateUuid();
  r1.scene_id = scene_.scene_id;
  r1.coordinate_frame = "reconstruction_0";
  r1.status = "reconstructing";
  r1.created_at_ns = 1000;
  r1.document_json = "{}";
  db_.AddReconstruction(r1);

  const auto latest = db_.QueryLatestReconstructionByScene(scene_.scene_id);
  EXPECT_FALSE(latest.has_value());
}

TEST_F(ReconstructionDbTest, FindReconstructionsByScene) {
  ReconstructionRow r1;
  r1.reconstruction_id = GenerateUuid();
  r1.scene_id = scene_.scene_id;
  r1.coordinate_frame = "reconstruction_0";
  r1.status = "succeeded";
  r1.created_at_ns = 1000;
  r1.document_json = "{}";
  db_.AddReconstruction(r1);

  ReconstructionRow r2;
  r2.reconstruction_id = GenerateUuid();
  r2.scene_id = scene_.scene_id;
  r2.coordinate_frame = "reconstruction_0";
  r2.status = "superseded";
  r2.created_at_ns = 2000;
  r2.document_json = "{}";
  db_.AddReconstruction(r2);

  const auto all = db_.FindReconstructionsByScene(scene_.scene_id);
  EXPECT_EQ(all.size(), 2u);
  // Ordered by created_at_ns.
  EXPECT_EQ(all[0].created_at_ns, 1000);
  EXPECT_EQ(all[1].created_at_ns, 2000);
}

TEST_F(ReconstructionDbTest, FindReconstructionsBySceneEmpty) {
  const auto all = db_.FindReconstructionsByScene(scene_.scene_id);
  EXPECT_TRUE(all.empty());
}

TEST_F(ReconstructionDbTest, ReconstructionRowPreservesDocumentJson) {
  const std::string doc =
      R"({"schema_version":2,"cameras":[{"camera_id":1}],"images":[],"points3D":[]})";

  ReconstructionRow r;
  r.reconstruction_id = GenerateUuid();
  r.scene_id = scene_.scene_id;
  r.coordinate_frame = "reconstruction_0";
  r.status = "succeeded";
  r.created_at_ns = 5000;
  r.document_json = doc;
  db_.AddReconstruction(r);

  const auto found = db_.QueryLatestReconstructionByScene(scene_.scene_id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->document_json, doc);
}

TEST_F(ReconstructionDbTest, InsertReadOnlyThrows) {
  // Close writable, reopen read-only.
  db_.Close();
  auto ro = MetadataDb::OpenReadOnly(path_);

  ReconstructionRow row;
  row.reconstruction_id = GenerateUuid();
  row.scene_id = scene_.scene_id;
  row.coordinate_frame = "reconstruction_0";
  row.status = "succeeded";
  row.created_at_ns = 1000;
  row.document_json = "{}";
  EXPECT_THROW(ro.AddReconstruction(row), StorageError);
}

TEST_F(ReconstructionDbTest, DifferentScenesAreIsolated) {
  // Create a second project with its own scene.
  const auto project_b = GenerateUuid();
  db_.InsertProject(project_b, "project_b", 1, "{}", 1000, "ENU", "world",
                    "{}", "{}");
  const auto scene_b = db_.FindOrCreateScene(project_b, "scene_b", "{}", 3000);

  ReconstructionRow r1;
  r1.reconstruction_id = GenerateUuid();
  r1.scene_id = scene_.scene_id;
  r1.coordinate_frame = "reconstruction_0";
  r1.status = "succeeded";
  r1.created_at_ns = 1000;
  r1.document_json = "{}";
  db_.AddReconstruction(r1);

  ReconstructionRow r2;
  r2.reconstruction_id = GenerateUuid();
  r2.scene_id = scene_b.scene_id;
  r2.coordinate_frame = "reconstruction_0";
  r2.status = "succeeded";
  r2.created_at_ns = 2000;
  r2.document_json = "{}";
  db_.AddReconstruction(r2);

  const auto latest_a = db_.QueryLatestReconstructionByScene(scene_.scene_id);
  ASSERT_TRUE(latest_a.has_value());
  EXPECT_EQ(latest_a->reconstruction_id, r1.reconstruction_id);

  const auto latest_b = db_.QueryLatestReconstructionByScene(scene_b.scene_id);
  ASSERT_TRUE(latest_b.has_value());
  EXPECT_EQ(latest_b->reconstruction_id, r2.reconstruction_id);

  const auto all_a = db_.FindReconstructionsByScene(scene_.scene_id);
  EXPECT_EQ(all_a.size(), 1u);
}

}  // namespace
}  // namespace spatial::core
