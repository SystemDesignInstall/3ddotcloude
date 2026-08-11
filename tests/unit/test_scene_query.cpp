#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/scene/query/scene_query.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"

namespace spatial::core {
namespace {

const Uuid kProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

void InsertTestProject(MetadataDb& db) {
  db.InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978", "local",
                   "{}", "{}");
}

void InsertTestSensor(MetadataDb& db, const Uuid& sensor_id) {
  SensorRow row;
  row.sensor_id = sensor_id;
  row.project_id = kProjectId;
  row.type = "camera";
  row.model = "sensor-model";
  db.RegisterSensor(row);
}

void AddWindowedCalibration(MetadataDb& db, const Uuid& sensor_id,
                            std::int64_t from_ns, std::int64_t to_ns) {
  CalibrationRow row;
  row.calibration_id = GenerateUuid();
  row.sensor_id = sensor_id;
  row.version = 1;
  row.calibration_time_ns = from_ns;
  row.source = "{}";
  row.intrinsics_json = "{\"fx\":1.0}";
  row.valid_from_ns = from_ns;
  row.valid_to_ns = to_ns;
  db.AddCalibration(row);
}

class SceneQueryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_sq_" + std::to_string(std::time(nullptr)) + "_" +
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

TEST_F(SceneQueryTest, QueryMapsRowsToDomainTypes) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);

  const Uuid session_id = GenerateUuid();
  CaptureSessionRow srow;
  srow.session_id = session_id;
  srow.project_id = kProjectId;
  srow.name = "scan-a";
  srow.started_at_ns = 100;
  srow.ended_at_ns = 900;
  srow.status = "closed";
  srow.provenance_json = "{\"source\":\"fixture\"}";
  db.InsertCaptureSession(srow);

  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(db, sensor_id);
  AddWindowedCalibration(db, sensor_id, 100, 900);

  const auto scene = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);

  const Uuid frame_id = GenerateUuid();
  FrameRow frow;
  frow.frame_id = frame_id;
  frow.scene_id = scene.scene_id;
  frow.session_id = session_id;
  frow.timestamp_ns = 500;
  frow.sequence_index = 3;
  frow.sensor_id = sensor_id;
  frow.pose_ref = {};
  db.InsertFrame(frow);

  const Uuid obs_id = GenerateUuid();
  ObservationRow orow;
  orow.observation_id = obs_id;
  orow.scene_id = scene.scene_id;
  orow.sensor_id = sensor_id;
  orow.frame_id = frame_id;
  orow.session_id = session_id;
  orow.timestamp_ns = 500;
  orow.type = "image";
  orow.artifact_ref = FormatUuid(obs_id);
  orow.source_json = "{\"producer\":\"importer\"}";
  db.InsertObservation(orow);

  ObservationPayloadRow prow;
  prow.observation_id = obs_id;
  prow.width = 1280;
  prow.height = 720;
  prow.pixel_format = "rgb8";
  db.InsertObservationPayload(prow);

  SceneQuery q(db);

  const auto session = q.FindCaptureSession(session_id);
  ASSERT_TRUE(session.has_value());
  EXPECT_EQ(session->name, "scan-a");
  EXPECT_EQ(session->project_id, kProjectId);
  EXPECT_EQ(session->started_at, TimestampNs(100));
  EXPECT_EQ(session->ended_at, TimestampNs(900));
  EXPECT_EQ(session->status, "closed");

  const auto by_project = q.FindSceneByProject(kProjectId);
  ASSERT_TRUE(by_project.has_value());
  EXPECT_EQ(by_project->scene_id, scene.scene_id);
  EXPECT_EQ(by_project->name, "scene");

  const auto session_scene = q.SessionScene(session_id);
  ASSERT_TRUE(session_scene.has_value());
  EXPECT_EQ(session_scene->scene_id, scene.scene_id);
  EXPECT_FALSE(q.SessionScene(GenerateUuid()).has_value());

  const auto sensor = q.ResolveSensor(sensor_id);
  ASSERT_TRUE(sensor.has_value());
  EXPECT_EQ(sensor->model, "sensor-model");
  EXPECT_TRUE(sensor->has_calibration);
  EXPECT_EQ(sensor->project_id, kProjectId);
  EXPECT_FALSE(q.ResolveSensor(GenerateUuid()).has_value());

  const auto cal = q.ResolveCalibrationAt(sensor_id, TimestampNs(500));
  ASSERT_TRUE(cal.has_value());
  EXPECT_EQ(cal->version, 1);
  EXPECT_EQ(cal->valid_from, std::optional<TimestampNs>(TimestampNs(100)));
  EXPECT_EQ(cal->valid_to, std::optional<TimestampNs>(TimestampNs(900)));
  EXPECT_FALSE(q.ResolveCalibrationAt(sensor_id, TimestampNs(950)).has_value());

  const auto frames = q.Frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].frame_id, frame_id);
  EXPECT_EQ(frames[0].session_id, session_id);
  EXPECT_EQ(frames[0].timestamp_ns, TimestampNs(500));
  EXPECT_EQ(frames[0].sequence_index, 3);
  EXPECT_TRUE(IsNil(frames[0].pose_ref));

  const auto by_session = q.FramesBySession(session_id);
  ASSERT_EQ(by_session.size(), 1u);
  EXPECT_EQ(by_session[0].frame_id, frame_id);
  const auto by_sensor = q.FramesBySensor(sensor_id);
  ASSERT_EQ(by_sensor.size(), 1u);
  EXPECT_EQ(by_sensor[0].frame_id, frame_id);
  const auto ranged = q.FramesInTimeRange(TimestampNs(100), TimestampNs(600));
  ASSERT_EQ(ranged.size(), 1u);
  EXPECT_TRUE(q.FramesInTimeRange(TimestampNs(600), TimestampNs(700)).empty())
      << "half-open range must exclude ts >= to";

  const auto obs = q.Observations();
  ASSERT_EQ(obs.size(), 1u);
  EXPECT_EQ(obs[0].observation_id, obs_id);
  EXPECT_EQ(obs[0].frame_id, frame_id);
  EXPECT_EQ(obs[0].artifact_ref, obs_id);
  EXPECT_EQ(obs[0].width, 1280);
  EXPECT_EQ(obs[0].height, 720);
  EXPECT_EQ(obs[0].pixel_format, "rgb8");

  const auto obs_by_frame = q.ObservationsByFrame(frame_id);
  ASSERT_EQ(obs_by_frame.size(), 1u);
  EXPECT_EQ(obs_by_frame[0].observation_id, obs_id);
  const auto obs_by_session = q.ObservationsBySession(session_id);
  ASSERT_EQ(obs_by_session.size(), 1u);
  EXPECT_EQ(obs_by_session[0].observation_id, obs_id);
  const auto obs_by_sensor = q.ObservationsBySensor(sensor_id);
  ASSERT_EQ(obs_by_sensor.size(), 1u);
  EXPECT_EQ(obs_by_sensor[0].observation_id, obs_id);
  const auto obs_ranged =
      q.ObservationsInTimeRange(TimestampNs(100), TimestampNs(600));
  ASSERT_EQ(obs_ranged.size(), 1u);
  EXPECT_TRUE(q.ObservationsInTimeRange(TimestampNs(600), TimestampNs(700))
                  .empty());
}

TEST_F(SceneQueryTest, QueryWorksAgainstReadOnlyDatabase) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const auto scene = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);
  db.Close();

  auto ro = MetadataDb::OpenReadOnly(path_);
  SceneQuery q(ro);
  const auto found = q.FindSceneByProject(kProjectId);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->scene_id, scene.scene_id);
  EXPECT_TRUE(q.Frames().empty());
  EXPECT_TRUE(q.Observations().empty());
  EXPECT_FALSE(q.SessionScene(GenerateUuid()).has_value());
}

TEST_F(SceneQueryTest, NonImageObservationsAreExcluded) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const auto scene = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);

  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(db, sensor_id);
  const Uuid session_id = GenerateUuid();
  CaptureSessionRow srow;
  srow.session_id = session_id;
  srow.project_id = kProjectId;
  srow.name = "s";
  db.InsertCaptureSession(srow);

  ObservationRow orow;
  orow.observation_id = GenerateUuid();
  orow.scene_id = scene.scene_id;
  orow.sensor_id = sensor_id;
  orow.frame_id = {};
  orow.session_id = session_id;
  orow.timestamp_ns = 500;
  orow.type = "lidar";
  orow.artifact_ref = "lidar-scan-1";
  db.InsertObservation(orow);

  SceneQuery q(db);
  EXPECT_TRUE(q.Observations().empty());
  EXPECT_TRUE(q.ObservationsBySession(session_id).empty());
  EXPECT_TRUE(q.ObservationsBySensor(sensor_id).empty());
}

TEST_F(SceneQueryTest, ArtifactHashBridgesUuidToContentHash) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);

  const Uuid artifact_uuid = GenerateUuid();
  ArtifactIndexRow row;
  row.artifact_id = artifact_uuid;
  row.content_hash = "deadbeef0123456789";
  row.type = "image";
  row.producer_json = "{}";
  row.created_at_ns = 1000;
  db.UpsertArtifact(row);

  SceneQuery q(db);
  const auto hash = q.ArtifactHash(artifact_uuid);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(*hash, "deadbeef0123456789");
  EXPECT_FALSE(q.ArtifactHash(GenerateUuid()).has_value());
}

TEST_F(SceneQueryTest, ArtifactHashMatchesAuthoritativeCasHash) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  ArtifactStore store(root_ / "artifacts", db);

  const std::vector<std::uint8_t> image_bytes = {'I', 'M', 'G', '0', '1'};
  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "image";
  manifest.producer = {"spatial-platform", "0.1.0", "test"};
  manifest.creation_timestamp = "2026-01-01T00:00:00Z";
  manifest.file_size = static_cast<std::int64_t>(image_bytes.size());
  const auto written = store.Put(image_bytes, manifest);

  // RFC-0007 §4 read-boundary bridge: UUID -> authoritative content hash ->
  // CAS bytes. The hash the boundary returns is exactly the one the CAS
  // stored, and the CAS returns exactly the original bytes (ADR-010).
  SceneQuery q(db);
  const auto hash = q.ArtifactHash(written.artifact_uuid);
  ASSERT_TRUE(hash.has_value());
  EXPECT_EQ(*hash, written.content_hash);
  const auto bytes = store.Get(*hash);
  ASSERT_TRUE(bytes.has_value());
  EXPECT_EQ(*bytes, image_bytes);
}

TEST_F(SceneQueryTest, SceneVersionReadsMapRowsToDomainType) {
  auto db = MetadataDb::Create(path_);
  InsertTestProject(db);
  const auto scene = db.FindOrCreateScene(kProjectId, "scene", "{}", 1000);
  const auto v2 = db.CreateSceneVersion(scene.scene_id, "imported", "{}", 2000);

  SceneQuery q(db);
  const auto version = q.SceneVersion(v2.version_id);
  ASSERT_TRUE(version.has_value());
  EXPECT_EQ(version->version_id, v2.version_id);
  EXPECT_EQ(version->scene_id, scene.scene_id);
  EXPECT_EQ(version->parent_version_id, scene.version_id);
  EXPECT_EQ(version->stage, "imported");
  EXPECT_EQ(version->created_at_ns, 2000);
  EXPECT_FALSE(q.SceneVersion(GenerateUuid()).has_value());

  const auto versions = q.SceneVersions(scene.scene_id);
  ASSERT_EQ(versions.size(), 2u);
  EXPECT_EQ(versions[0].stage, "created");
  EXPECT_EQ(versions[1].stage, "imported");
}

}  // namespace
}  // namespace spatial::core
