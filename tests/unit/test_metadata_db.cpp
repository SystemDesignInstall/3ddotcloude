#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include <sqlite3.h>

#include "core/errors/project_error.h"
#include "core/storage/metadata_db.h"

namespace spatial::core {
namespace {

class MetadataDbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_mdb_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    path_ = root_ / "project.db";
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
  std::filesystem::path path_;
};

TEST_F(MetadataDbTest, CreateAppliesMigrations) {
  auto db = MetadataDb::Create(path_);
  ASSERT_TRUE(db.IsOpen());
  EXPECT_FALSE(db.read_only());
  EXPECT_TRUE(std::filesystem::exists(path_));
  // Opening again must not re-apply.
  auto db2 = MetadataDb::Open(path_);
  EXPECT_EQ(db2.ApplyMigrations(), 0u);
}

TEST_F(MetadataDbTest, MigrationIdempotent) {
  auto db = MetadataDb::Create(path_);
  EXPECT_EQ(db.ApplyMigrations(), 0u);
  auto db2 = MetadataDb::Open(path_);
  EXPECT_EQ(db2.ApplyMigrations(), 0u);
}

TEST_F(MetadataDbTest, InsertAndFindArtifact) {
  auto db = MetadataDb::Create(path_);
  const auto uuid = GenerateUuid();
  ArtifactIndexRow row;
  row.artifact_id = uuid;
  row.content_hash = "abc123";
  row.type = "pointcloud";
  row.producer_json = "{}";
  row.created_at_ns = 12345;
  row.coordinate_frame = "scene";
  row.unit = "meter";
  row.file_size = 10;
  row.mime_type = "application/x-ply";
  db.UpsertArtifact(row);

  const auto found = db.FindArtifactByHash("abc123");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->artifact_id, uuid);
  EXPECT_EQ(found->type, "pointcloud");
  EXPECT_EQ(found->file_size, 10);
  EXPECT_EQ(found->validation_status, "valid");

  EXPECT_FALSE(db.FindArtifactByHash("zzz").has_value());
  const auto by_type = db.FindArtifactsByType("pointcloud");
  ASSERT_EQ(by_type.size(), 1u);
  EXPECT_EQ(by_type[0].content_hash, "abc123");
}

TEST_F(MetadataDbTest, DedupUpsertKeepsOneRow) {
  auto db = MetadataDb::Create(path_);
  const auto uuid = GenerateUuid();
  ArtifactIndexRow row;
  row.artifact_id = uuid;
  row.content_hash = "hash1";
  row.type = "mesh";
  row.producer_json = "{}";
  db.UpsertArtifact(row);

  ArtifactIndexRow row2 = row;
  row2.file_size = 99;
  db.UpsertArtifact(row2);

  EXPECT_EQ(db.FindArtifactsByType("mesh").size(), 1u);
  EXPECT_EQ(db.FindArtifactByHash("hash1")->file_size, 99);
}

TEST_F(MetadataDbTest, RecordDependency) {
  auto db = MetadataDb::Create(path_);
  EXPECT_NO_THROW(db.RecordDependency("in1", "out1", "input"));
  EXPECT_NO_THROW(db.RecordDependency("in2", "out1", "input"));
}

TEST_F(MetadataDbTest, ReadOnlyRejectsWrites) {
  auto db = MetadataDb::Create(path_);
  db.Close();
  auto ro = MetadataDb::OpenReadOnly(path_);
  EXPECT_TRUE(ro.read_only());
  ArtifactIndexRow row;
  row.artifact_id = GenerateUuid();
  row.content_hash = "h";
  row.type = "t";
  row.producer_json = "{}";
  EXPECT_THROW(ro.UpsertArtifact(row), StorageError);
}

TEST_F(MetadataDbTest, OpenMissingThrows) {
  EXPECT_THROW(MetadataDb::Open(root_ / "nope.db"), StorageError);
}

namespace {

const Uuid kProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

void InsertTestProject(MetadataDb& db) {
  db.InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978", "local",
                   "{}", "{}");
}

void InsertTestSensor(MetadataDb& db, const Uuid& sensor_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO sensors (sensor_id, project_id, type)"
                    " VALUES (?, ?, 'camera')";
  ASSERT_EQ(sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
  sqlite3_bind_blob(stmt, 1, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, kProjectId.data(),
                    static_cast<int>(kProjectId.size()), SQLITE_TRANSIENT);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
}

void InsertTestCalibration(MetadataDb& db, const Uuid& sensor_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO calibrations (calibration_id, sensor_id, version)"
      " VALUES (?, ?, 1)";
  ASSERT_EQ(sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
  const Uuid cal_id = GenerateUuid();
  sqlite3_bind_blob(stmt, 1, cal_id.data(), static_cast<int>(cal_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
}

}  // namespace

TEST_F(MetadataDbTest, CaptureSessionRoundTrip) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  CaptureSessionRow row;
  row.session_id = GenerateUuid();
  row.project_id = kProjectId;
  row.name = "session-001";
  row.started_at_ns = 123456;
  row.status = "open";
  db.InsertCaptureSession(row);

  const auto found = db.FindSession(row.session_id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, row.session_id);
  EXPECT_FALSE(db.FindSession(GenerateUuid()).has_value());
}

TEST_F(MetadataDbTest, FindOrCreateSceneCreatesOnce) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const auto s1 = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);
  EXPECT_FALSE(IsNil(s1.scene_id));
  EXPECT_FALSE(IsNil(s1.version_id));
  EXPECT_EQ(s1.project_id, kProjectId);
  EXPECT_EQ(s1.stage, "created");
  // Second call resolves the same scene, does not duplicate.
  const auto s2 = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);
  EXPECT_EQ(s2.scene_id, s1.scene_id);
  EXPECT_EQ(s2.version_id, s1.version_id);
}

TEST_F(MetadataDbTest, CreateSceneVersionChainsAndAdvances) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const auto scene = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);
  const auto v2 = db.CreateSceneVersion(scene.scene_id, "imported", "{}", 2000);
  EXPECT_EQ(v2.scene_id, scene.scene_id);
  EXPECT_EQ(v2.parent_version_id, scene.version_id);
  EXPECT_EQ(v2.stage, "imported");

  const auto s2 = db.FindOrCreateScene(kProjectId, "scene", "{}", 3000);
  EXPECT_EQ(s2.version_id, v2.version_id);
}

TEST_F(MetadataDbTest, FrameObservationPayloadRoundTrip) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(db, sensor_id);
  const auto scene = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);
  const Uuid session_id = GenerateUuid();
  CaptureSessionRow srow;
  srow.session_id = session_id;
  srow.project_id = kProjectId;
  srow.name = "s";
  db.InsertCaptureSession(srow);

  const Uuid frame_id = GenerateUuid();
  FrameRow frow;
  frow.frame_id = frame_id;
  frow.scene_id = scene.scene_id;
  frow.session_id = session_id;
  frow.timestamp_ns = 5000;
  frow.sequence_index = 0;
  frow.sensor_id = sensor_id;
  frow.pose_ref = {};
  EXPECT_NO_THROW(db.InsertFrame(frow));

  const Uuid obs_id = GenerateUuid();
  ObservationRow orow;
  orow.observation_id = obs_id;
  orow.scene_id = scene.scene_id;
  orow.sensor_id = sensor_id;
  orow.frame_id = frame_id;
  orow.session_id = session_id;
  orow.timestamp_ns = 5000;
  orow.type = "image";
  orow.artifact_ref = "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d";
  EXPECT_NO_THROW(db.InsertObservation(orow));

  ObservationPayloadRow prow;
  prow.observation_id = obs_id;
  prow.width = 1920;
  prow.height = 1080;
  prow.pixel_format = "rgb8";
  EXPECT_NO_THROW(db.InsertObservationPayload(prow));

  // Re-inserting the same observation id violates PK; frames/obs are immutable
  // append-only records (PPS-0001 §5.3).
  EXPECT_THROW(db.InsertFrame(frow), SchemaError);
  EXPECT_THROW(db.InsertObservation(orow), SchemaError);
}

TEST_F(MetadataDbTest, SensorResolutionTracksCalibration) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(db, sensor_id);

  const auto bare = db.FindSensor(sensor_id);
  ASSERT_TRUE(bare.has_value());
  EXPECT_EQ(bare->type, "camera");
  EXPECT_FALSE(bare->has_calibration);

  InsertTestCalibration(db, sensor_id);
  const auto cal = db.FindSensor(sensor_id);
  ASSERT_TRUE(cal.has_value());
  EXPECT_TRUE(cal->has_calibration);

  EXPECT_FALSE(db.FindSensor(GenerateUuid()).has_value());
}

TEST_F(MetadataDbTest, SceneVersionWithoutSceneThrows) {
  auto db = MetadataDb::Create(path_);
  EXPECT_THROW(db.CreateSceneVersion(GenerateUuid(), "imported", "{}", 1),
               SchemaError);
}

}  // namespace
}  // namespace spatial::core
