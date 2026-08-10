#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>

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

TEST_F(MetadataDbTest, RegisterSensorRoundTrip) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  SensorRow row;
  row.sensor_id = GenerateUuid();
  row.project_id = kProjectId;
  row.type = "camera";
  row.manufacturer = "Acme";
  row.model = "Cam-1";
  row.serial_number = "SN-42";
  row.time_domain = "device";
  row.source_json = R"({"app":"spatial","version":"0.1"})";
  row.status = "active";
  db.RegisterSensor(row);

  const auto found = db.FindSensor(row.sensor_id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->sensor_id, row.sensor_id);
  EXPECT_EQ(found->project_id, kProjectId);
  EXPECT_EQ(found->type, "camera");
  EXPECT_EQ(found->manufacturer, "Acme");
  EXPECT_EQ(found->model, "Cam-1");
  EXPECT_EQ(found->serial_number, "SN-42");
  EXPECT_EQ(found->time_domain, "device");
  EXPECT_EQ(found->status, "active");
  EXPECT_TRUE(IsNil(found->calibration_id));
  EXPECT_FALSE(found->has_calibration);
}

TEST_F(MetadataDbTest, RegisterSensorDuplicateRejected) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  SensorRow row;
  row.sensor_id = GenerateUuid();
  row.project_id = kProjectId;
  row.type = "lidar";
  db.RegisterSensor(row);
  EXPECT_THROW(db.RegisterSensor(row), SchemaError);
  // The original row is intact.
  EXPECT_TRUE(db.FindSensor(row.sensor_id).has_value());
}

TEST_F(MetadataDbTest, UpdateSensorMetadataKeepsIdentity) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  SensorRow row;
  row.sensor_id = GenerateUuid();
  row.project_id = kProjectId;
  row.type = "imu";
  row.manufacturer = "old";
  row.status = "active";
  db.RegisterSensor(row);

  SensorMetadataUpdate update;
  update.manufacturer = "new";
  update.model = "Gyro-9";
  update.status = "retired";
  db.UpdateSensorMetadata(row.sensor_id, update);

  const auto found = db.FindSensor(row.sensor_id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->sensor_id, row.sensor_id);
  EXPECT_EQ(found->type, "imu");  // type is identity-adjacent, untouched
  EXPECT_EQ(found->manufacturer, "new");
  EXPECT_EQ(found->model, "Gyro-9");
  EXPECT_EQ(found->status, "retired");
  EXPECT_TRUE(found->serial_number.empty());

  EXPECT_THROW(
      db.UpdateSensorMetadata(GenerateUuid(), SensorMetadataUpdate{}),
      SchemaError);
}

namespace {

SensorRow MakeSensor(const Uuid& id) {
  SensorRow row;
  row.sensor_id = id;
  row.project_id = kProjectId;
  row.type = "camera";
  row.status = "active";
  return row;
}

CalibrationRow MakeCalibration(const Uuid& sensor_id,
                               std::int64_t valid_from,
                               std::optional<std::int64_t> valid_to) {
  CalibrationRow row;
  row.calibration_id = GenerateUuid();
  row.sensor_id = sensor_id;
  row.calibration_time_ns = valid_from;
  row.valid_from_ns = valid_from;
  row.valid_to_ns = valid_to;
  return row;
}

}  // namespace

TEST_F(MetadataDbTest, AddCalibrationAppendsVersionsAndLatestPointer) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const Uuid sensor_id = GenerateUuid();
  db.RegisterSensor(MakeSensor(sensor_id));

  const auto c1 = MakeCalibration(sensor_id, 100, 200);
  db.AddCalibration(c1);
  const auto c2 = MakeCalibration(sensor_id, 300, std::nullopt);
  db.AddCalibration(c2);

  const auto found = db.FindSensor(sensor_id);
  ASSERT_TRUE(found.has_value());
  EXPECT_TRUE(found->has_calibration);
  EXPECT_EQ(found->calibration_id, c2.calibration_id);  // latest pointer

  // Versions are append-only and monotonically increasing.
  const auto at100 = db.ResolveCalibrationAt(sensor_id, 100);
  ASSERT_TRUE(at100.has_value());
  EXPECT_EQ(at100->calibration_id, c1.calibration_id);
  EXPECT_EQ(at100->version, 1);

  const auto at300 = db.ResolveCalibrationAt(sensor_id, 300);
  ASSERT_TRUE(at300.has_value());
  EXPECT_EQ(at300->calibration_id, c2.calibration_id);
  EXPECT_EQ(at300->version, 2);
  EXPECT_FALSE(at300->valid_to_ns.has_value());  // open-ended
}

TEST_F(MetadataDbTest, AddCalibrationRejectsMalformedInterval) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const Uuid sensor_id = GenerateUuid();
  db.RegisterSensor(MakeSensor(sensor_id));

  CalibrationRow no_start = MakeCalibration(sensor_id, 100, 200);
  no_start.valid_from_ns.reset();
  EXPECT_THROW(db.AddCalibration(no_start), CalibrationError);

  CalibrationRow inverted = MakeCalibration(sensor_id, 200, 200);
  EXPECT_THROW(db.AddCalibration(inverted), CalibrationError);

  CalibrationRow reversed = MakeCalibration(sensor_id, 300, 100);
  EXPECT_THROW(db.AddCalibration(reversed), CalibrationError);
}

TEST_F(MetadataDbTest, AddCalibrationRejectsOverlap) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const Uuid sensor_id = GenerateUuid();
  db.RegisterSensor(MakeSensor(sensor_id));

  db.AddCalibration(MakeCalibration(sensor_id, 100, 200));
  // Overlaps [100, 200).
  EXPECT_THROW(db.AddCalibration(MakeCalibration(sensor_id, 150, 250)),
               CalibrationError);
  EXPECT_THROW(db.AddCalibration(MakeCalibration(sensor_id, 50, 150)),
               CalibrationError);
  // Adjacent half-open intervals [100,200) / [200,300) do not overlap.
  EXPECT_NO_THROW(db.AddCalibration(MakeCalibration(sensor_id, 200, 300)));
}

TEST_F(MetadataDbTest, ResolveCalibrationAtIsHalfOpenAndGapAware) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const Uuid sensor_id = GenerateUuid();
  db.RegisterSensor(MakeSensor(sensor_id));

  db.AddCalibration(MakeCalibration(sensor_id, 100, 200));
  db.AddCalibration(MakeCalibration(sensor_id, 300, std::nullopt));

  // Half-open: valid_from inclusive, valid_to exclusive.
  EXPECT_TRUE(db.ResolveCalibrationAt(sensor_id, 100).has_value());
  EXPECT_TRUE(db.ResolveCalibrationAt(sensor_id, 199).has_value());
  EXPECT_FALSE(db.ResolveCalibrationAt(sensor_id, 200).has_value());
  // Before the first interval and in the [200, 300) gap.
  EXPECT_FALSE(db.ResolveCalibrationAt(sensor_id, 99).has_value());
  EXPECT_FALSE(db.ResolveCalibrationAt(sensor_id, 250).has_value());
  // Open-ended interval matches everything >= valid_from.
  EXPECT_TRUE(db.ResolveCalibrationAt(sensor_id, 300).has_value());
  EXPECT_TRUE(db.ResolveCalibrationAt(
      sensor_id, std::numeric_limits<std::int64_t>::max()).has_value());
  // Uncalibrated and unknown sensors resolve to nullopt, never a guess.
  const Uuid uncalibrated = GenerateUuid();
  db.RegisterSensor(MakeSensor(uncalibrated));
  EXPECT_FALSE(db.ResolveCalibrationAt(uncalibrated, 100).has_value());
  EXPECT_FALSE(db.ResolveCalibrationAt(GenerateUuid(), 100).has_value());
}

TEST_F(MetadataDbTest, SceneVersionWithoutSceneThrows) {
  auto db = MetadataDb::Create(path_);
  EXPECT_THROW(db.CreateSceneVersion(GenerateUuid(), "imported", "{}", 1),
               SchemaError);
}

TEST_F(MetadataDbTest, ReadSurfaceResolvesSessionsAndScenes) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const Uuid session_id = GenerateUuid();
  CaptureSessionRow srow;
  srow.session_id = session_id;
  srow.project_id = kProjectId;
  srow.name = "session-query";
  srow.started_at_ns = 100;
  srow.status = "closed";
  db.InsertCaptureSession(srow);

  const auto session = db.FindCaptureSession(session_id);
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(session->session_id, session_id);
  EXPECT_EQ(session->project_id, kProjectId);
  EXPECT_EQ(session->name, "session-query");
  EXPECT_EQ(session->status, "closed");
  EXPECT_FALSE(db.FindCaptureSession(GenerateUuid()).has_value());

  const auto scene = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);
  const auto by_project = db.FindSceneByProject(kProjectId);
  ASSERT_TRUE(by_project.has_value());
  EXPECT_EQ(by_project->scene_id, scene.scene_id);
  EXPECT_EQ(by_project->version_id, scene.version_id);
  EXPECT_EQ(by_project->project_id, kProjectId);
  EXPECT_FALSE(db.FindSceneByProject(GenerateUuid()).has_value());
}

TEST_F(MetadataDbTest, FrameAndObservationReadQueriesReturnSubsets) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const auto scene = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);

  const Uuid session_a = GenerateUuid();
  const Uuid session_b = GenerateUuid();
  for (const auto& sid : {session_a, session_b}) {
    CaptureSessionRow srow;
    srow.session_id = sid;
    srow.project_id = kProjectId;
    srow.name = "s";
    db.InsertCaptureSession(srow);
  }

  const Uuid sensor_1 = GenerateUuid();
  const Uuid sensor_2 = GenerateUuid();
  InsertTestSensor(db, sensor_1);
  InsertTestSensor(db, sensor_2);

  struct Record {
    Uuid id;
    std::int64_t ts;
  };
  std::vector<Record> frames;
  std::vector<Record> observations;

  const auto add_frame = [&](const Uuid& session_id, const Uuid& sensor_id,
                             std::int64_t ts) {
    const Uuid frame_id = GenerateUuid();
    FrameRow frow;
    frow.frame_id = frame_id;
    frow.scene_id = scene.scene_id;
    frow.session_id = session_id;
    frow.timestamp_ns = ts;
    frow.sequence_index = 0;
    frow.sensor_id = sensor_id;
    frow.pose_ref = {};
    db.InsertFrame(frow);
    frames.push_back({frame_id, ts});

    const Uuid obs_id = GenerateUuid();
    ObservationRow orow;
    orow.observation_id = obs_id;
    orow.scene_id = scene.scene_id;
    orow.sensor_id = sensor_id;
    orow.frame_id = frame_id;
    orow.session_id = session_id;
    orow.timestamp_ns = ts;
    orow.type = "image";
    orow.artifact_ref = FormatUuid(obs_id);
    db.InsertObservation(orow);
    observations.push_back({obs_id, ts});
  };

  // f1/o1: session A, sensor 1, ts 100; f2/o2: session A, sensor 2, ts 200;
  // f3/o3: session B, sensor 1, ts 300.
  add_frame(session_a, sensor_1, 100);
  add_frame(session_a, sensor_2, 200);
  add_frame(session_b, sensor_1, 300);

  const auto ids = [](const auto& rows) {
    std::vector<Uuid> out;
    for (const auto& r : rows) out.push_back(r.frame_id);
    return out;
  };
  const auto obs_ids = [](const auto& rows) {
    std::vector<Uuid> out;
    for (const auto& r : rows) out.push_back(r.observation_id);
    return out;
  };
  const auto contains = [](const auto& vec, const Uuid& id) {
    return std::find(vec.begin(), vec.end(), id) != vec.end();
  };

  const auto all_frames = db.FindFramesByScene(scene.scene_id);
  ASSERT_EQ(all_frames.size(), 3u);

  const auto session_a_frames = db.FindFramesBySession(session_a);
  ASSERT_EQ(session_a_frames.size(), 2u);
  EXPECT_TRUE(contains(ids(session_a_frames), frames[0].id));
  EXPECT_TRUE(contains(ids(session_a_frames), frames[1].id));

  const auto sensor_1_frames = db.FindFramesBySensor(sensor_1);
  ASSERT_EQ(sensor_1_frames.size(), 2u);
  EXPECT_TRUE(contains(ids(sensor_1_frames), frames[0].id));
  EXPECT_TRUE(contains(ids(sensor_1_frames), frames[2].id));

  // Half-open time range [100, 300): ts 300 is excluded.
  const auto ranged = db.FindFramesInTimeRange(100, 300);
  ASSERT_EQ(ranged.size(), 2u);
  EXPECT_TRUE(contains(ids(ranged), frames[0].id));
  EXPECT_TRUE(contains(ids(ranged), frames[1].id));
  EXPECT_FALSE(contains(ids(ranged), frames[2].id));

  const auto frame_obs = db.FindObservationsByFrame(frames[0].id);
  ASSERT_EQ(frame_obs.size(), 1u);
  EXPECT_EQ(frame_obs[0].observation_id, observations[0].id);

  const auto session_a_obs = db.FindObservationsBySession(session_a);
  ASSERT_EQ(session_a_obs.size(), 2u);
  EXPECT_TRUE(contains(obs_ids(session_a_obs), observations[0].id));
  EXPECT_TRUE(contains(obs_ids(session_a_obs), observations[1].id));

  const auto sensor_1_obs = db.FindObservationsBySensor(sensor_1);
  ASSERT_EQ(sensor_1_obs.size(), 2u);
  EXPECT_TRUE(contains(obs_ids(sensor_1_obs), observations[0].id));
  EXPECT_TRUE(contains(obs_ids(sensor_1_obs), observations[2].id));

  const auto ranged_obs = db.FindObservationsInTimeRange(100, 300);
  ASSERT_EQ(ranged_obs.size(), 2u);
  EXPECT_TRUE(contains(obs_ids(ranged_obs), observations[0].id));
  EXPECT_TRUE(contains(obs_ids(ranged_obs), observations[1].id));

  const auto scene_obs = db.FindObservationsByScene(scene.scene_id);
  ASSERT_EQ(scene_obs.size(), 3u);
  EXPECT_EQ(scene_obs[0].type, "image");
  EXPECT_EQ(scene_obs[0].timestamp_ns, 100);
}

}  // namespace
}  // namespace spatial::core
