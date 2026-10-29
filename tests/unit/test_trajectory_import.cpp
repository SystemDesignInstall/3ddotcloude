#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/errors/project_error.h"
#include "core/project/project.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "importers/trajectory/trajectory_importer.h"
#include "schema_check.h"

namespace spatial::importers {
namespace {

using core::FormatUuid;
using core::GenerateUuid;
using core::Project;
using core::Uuid;
using core::ValidationError;
using nlohmann::json;

constexpr std::int64_t kTime1 = 1700000000123456789LL;
constexpr std::int64_t kTime2 = 1700000000124456789LL;

std::vector<std::uint8_t> Bytes(const std::string& value) {
  return {value.begin(), value.end()};
}

const std::string kTum =
    "# camera poses\n"
    "1700000000.123456789 0 0 0 0 0 0 1\n"
    "1700000000.124456789 3 4 0 0 0 0.7071068 0.7071068\n";

class TrajectoryImportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_trajectory_" + FormatUuid(GenerateUuid()));
    core::ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "trajectory";
    info.created_at = core::fs::Iso8601UtcNow();
    project_ = std::make_unique<Project>(Project::Create(root_ / "test.spx", info));
    sensor_id_ = GenerateUuid();
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(project_->db().db(),
        "INSERT INTO sensors (sensor_id, project_id, type, status) VALUES (?, ?, 'camera', 'active')",
        -1, &statement, nullptr), SQLITE_OK);
    sqlite3_bind_blob(statement, 1, sensor_id_.data(), 16, SQLITE_TRANSIENT);
    sqlite3_bind_blob(statement, 2, info.uuid.data(), 16, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
    scene_id_ = project_->db().FindOrCreateScene(info.uuid, "scene", "{}", 1).scene_id;
    session_id_ = AddSession();
    frame_1_ = AddFrame(session_id_, kTime1);
    frame_2_ = AddFrame(session_id_, kTime2);
    options_.session_id = session_id_;
    options_.sensor_id = sensor_id_;
    options_.coordinate_frame = "slam_local";
    options_.source_producer = "example_slam";
    options_.source_version = "2.1";
    options_.source_unit = TrajectoryLengthUnit::kMeters;
    options_.clock_domain = TrajectoryClockDomain::kPlatform;
    options_.pose_frame = TrajectoryPoseFrame::kCamera;
  }

  void TearDown() override {
    project_.reset();
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  Uuid AddSession() {
    core::CaptureSessionRow row;
    row.session_id = GenerateUuid();
    row.project_id = project_->info().uuid;
    row.name = "capture";
    project_->db().InsertCaptureSession(row);
    return row.session_id;
  }

  Uuid AddFrame(const Uuid& session, std::int64_t timestamp) {
    core::FrameRow row;
    row.frame_id = GenerateUuid();
    row.scene_id = scene_id_;
    row.session_id = session;
    row.sensor_id = sensor_id_;
    row.timestamp_ns = timestamp;
    row.properties_json = "{}";
    project_->db().InsertFrame(row);
    return row.frame_id;
  }

  TrajectoryImportResult Import(const std::string& text) {
    return ImportTumCameraTrajectory(project_->artifacts(), project_->db(),
                                     project_->info().uuid, Bytes(text), options_);
  }

  std::filesystem::path root_;
  std::unique_ptr<Project> project_;
  Uuid scene_id_{};
  Uuid session_id_{};
  Uuid sensor_id_{};
  Uuid frame_1_{};
  Uuid frame_2_{};
  TrajectoryImportOptions options_;
};

TEST_F(TrajectoryImportTest, PublishesExactFrameLinksAndUnverifiedProvenance) {
  const auto result = Import(kTum);
  EXPECT_FALSE(result.reused);
  EXPECT_EQ(result.scene_id, scene_id_);
  EXPECT_EQ(result.node_count, 2);
  EXPECT_DOUBLE_EQ(result.total_distance_m, 5.0);
  EXPECT_EQ(result.source_content_hash, core::Sha256Hex(Bytes(kTum)));
  const auto source = project_->artifacts().Get(result.source_content_hash);
  ASSERT_TRUE(source.has_value());
  EXPECT_EQ(*source, Bytes(kTum));
  const auto data = project_->artifacts().Get(result.content_hash);
  ASSERT_TRUE(data.has_value());
  const auto doc = json::parse(data->begin(), data->end());
  EXPECT_EQ(doc.size(), 3u);
  EXPECT_FALSE(doc.contains("metric_basis"));
  ASSERT_EQ(doc["nodes"].size(), 2u);
  EXPECT_EQ(doc["nodes"][0]["frame_id"], FormatUuid(frame_1_));
  EXPECT_EQ(doc["nodes"][1]["frame_id"], FormatUuid(frame_2_));
  EXPECT_EQ(doc["nodes"][0]["timestamp_ns"], kTime1);
  EXPECT_EQ(doc["nodes"][1]["timestamp_ns"], kTime2);
  EXPECT_NEAR(doc["nodes"][1]["rotation_xyzw"][3].get<double>(), 0.7071067811865476, 1e-12);
  std::ifstream file(SPATIAL_TRAJECTORY_SCHEMA_JSON);
  ASSERT_TRUE(file.good());
  auto schema = json::parse(file);
  schema["properties"]["nodes"]["items"] = schema["definitions"]["PoseNode"];
  std::vector<std::string> violations;
  CheckNode(schema, doc, "$", &violations);
  EXPECT_TRUE(violations.empty());
  const auto manifest = project_->artifacts().ReadManifest(result.artifact_id);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->type, "trajectory");
  EXPECT_EQ(manifest->coordinate_frame, "slam_local");
  EXPECT_EQ(manifest->unit, "meters");
  EXPECT_EQ(manifest->validation_status, "unverified");
  ASSERT_EQ(manifest->input_artifact_hashes.size(), 2u);
  EXPECT_EQ(manifest->input_artifact_hashes[0], result.source_content_hash);
  EXPECT_EQ(manifest->input_artifact_hashes[1], result.configuration_hash);
  const auto config_bytes = project_->artifacts().Get(result.configuration_hash);
  ASSERT_TRUE(config_bytes.has_value());
  const auto config = json::parse(config_bytes->begin(), config_bytes->end());
  EXPECT_EQ(config["source_unit"], "meters");
  EXPECT_EQ(config["clock_domain"], "platform");
  EXPECT_EQ(config["pose_frame"], "camera");
  EXPECT_EQ(config["source_producer"], "example_slam");
  EXPECT_EQ(config["source_version"], "2.1");
  EXPECT_EQ(config["frame_ids"][0], FormatUuid(frame_1_));
  const auto rows = project_->db().FindTrajectoriesBySession(session_id_);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().trajectory_id, result.trajectory_id);
  EXPECT_EQ(rows.front().document_json, std::string(data->begin(), data->end()));
  EXPECT_EQ(rows.front().total_duration_ns, 1000000);
}

TEST_F(TrajectoryImportTest, ReimportReusesTheSamePublishedTrajectory) {
  const auto first = Import(kTum);
  const auto count = project_->artifacts().PayloadCount();
  const auto second = Import(kTum);
  EXPECT_TRUE(second.reused);
  EXPECT_EQ(second.trajectory_id, first.trajectory_id);
  EXPECT_EQ(second.artifact_id, first.artifact_id);
  EXPECT_EQ(second.source_artifact_id, first.source_artifact_id);
  EXPECT_EQ(second.configuration_artifact_id, first.configuration_artifact_id);
  EXPECT_EQ(second.content_hash, first.content_hash);
  EXPECT_EQ(project_->artifacts().PayloadCount(), count);
  EXPECT_EQ(project_->db().FindTrajectoriesBySession(session_id_).size(), 1u);
}

TEST_F(TrajectoryImportTest, SameSourceInAnotherSessionDoesNotReplaceItsArtifactIndex) {
  const auto first = Import(kTum);
  const auto second_session = AddSession();
  AddFrame(second_session, kTime1);
  AddFrame(second_session, kTime2);
  options_.session_id = second_session;
  const auto second = Import(kTum);
  EXPECT_FALSE(second.reused);
  EXPECT_NE(second.trajectory_id, first.trajectory_id);
  EXPECT_EQ(second.source_content_hash, first.source_content_hash);
  EXPECT_EQ(second.source_artifact_id, first.source_artifact_id);
  const auto index = project_->db().FindArtifactByHash(first.source_content_hash);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->artifact_id, first.source_artifact_id);
  EXPECT_TRUE(project_->artifacts().ReadManifest(first.source_artifact_id).has_value());
}

TEST_F(TrajectoryImportTest, ExistingSourceOfAnotherTypeKeepsItsIdentity) {
  core::ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "existing_input";
  manifest.producer = {"existing", "1", "test"};
  manifest.unit = "none";
  const auto original = project_->artifacts().Put(Bytes(kTum), manifest);
  const auto result = Import(kTum);
  EXPECT_EQ(result.source_artifact_id, original.artifact_uuid);
  const auto index = project_->db().FindArtifactByHash(original.content_hash);
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(index->artifact_id, original.artifact_uuid);
  EXPECT_EQ(index->type, "existing_input");
  const auto existing_manifest = project_->artifacts().ReadManifest(original.artifact_uuid);
  ASSERT_TRUE(existing_manifest.has_value());
  EXPECT_EQ(existing_manifest->type, "existing_input");
}

TEST_F(TrajectoryImportTest, ScientificTimestampKeepsNanosecondPrecision) {
  const auto result = Import("1.700000000123456789e9 0 0 0 0 0 0 1\n");
  const auto bytes = project_->artifacts().Get(result.content_hash);
  ASSERT_TRUE(bytes.has_value());
  const auto doc = json::parse(bytes->begin(), bytes->end());
  EXPECT_EQ(doc["nodes"][0]["timestamp_ns"], kTime1);
  EXPECT_EQ(doc["nodes"][0]["frame_id"], FormatUuid(frame_1_));
}

TEST_F(TrajectoryImportTest, ReimportDetectsCorruptedSourceInsteadOfCreatingAnotherRow) {
  const auto first = Import(kTum);
  const auto path = project_->artifacts_root() / "cas" /
                    first.source_content_hash.substr(0, 2) / first.source_content_hash;
  core::fs::AtomicWrite(path, Bytes("corrupted"));
  EXPECT_THROW(Import(kTum), core::ArtifactError);
  EXPECT_EQ(project_->db().FindTrajectoriesBySession(session_id_).size(), 1u);
}

TEST_F(TrajectoryImportTest, ReimportDetectsCorruptedImportConfiguration) {
  const auto first = Import(kTum);
  const auto path = project_->artifacts_root() / "cas" /
                    first.configuration_hash.substr(0, 2) / first.configuration_hash;
  core::fs::AtomicWrite(path, Bytes("corrupted"));
  EXPECT_THROW(Import(kTum), core::ArtifactError);
  EXPECT_EQ(project_->db().FindTrajectoriesBySession(session_id_).size(), 1u);
}

TEST_F(TrajectoryImportTest, AChangedCoordinateFrameCreatesAnIndependentTrajectory) {
  const auto first = Import(kTum);
  options_.coordinate_frame = "another_slam_frame";
  const auto second = Import(kTum);
  EXPECT_FALSE(second.reused);
  EXPECT_NE(second.trajectory_id, first.trajectory_id);
  EXPECT_NE(second.configuration_hash, first.configuration_hash);
  EXPECT_EQ(second.source_artifact_id, first.source_artifact_id);
  EXPECT_EQ(project_->db().FindTrajectoriesBySession(session_id_).size(), 2u);
}

TEST_F(TrajectoryImportTest, DatabaseFailureRollsBackIndexesAndRetrySucceeds) {
  ASSERT_EQ(sqlite3_exec(project_->db().db(),
      "CREATE TRIGGER fail_trajectory BEFORE INSERT ON trajectories "
      "BEGIN SELECT RAISE(ABORT, 'simulated failure'); END;",
      nullptr, nullptr, nullptr), SQLITE_OK);
  EXPECT_THROW(Import(kTum), core::ProjectError);
  EXPECT_TRUE(project_->db().FindArtifactsByType("trajectory").empty());
  EXPECT_TRUE(project_->db().FindArtifactsByType("trajectory_source").empty());
  EXPECT_TRUE(project_->db().FindArtifactsByType("trajectory_import_configuration").empty());
  EXPECT_TRUE(project_->db().FindTrajectoriesBySession(session_id_).empty());
  ASSERT_EQ(sqlite3_exec(project_->db().db(), "DROP TRIGGER fail_trajectory",
                         nullptr, nullptr, nullptr), SQLITE_OK);
  const auto retried = Import(kTum);
  EXPECT_FALSE(retried.reused);
  EXPECT_TRUE(project_->artifacts().Get(retried.content_hash).has_value());
  EXPECT_EQ(project_->db().FindTrajectoriesBySession(session_id_).size(), 1u);
}

TEST_F(TrajectoryImportTest, MissingOrAmbiguousFrameFailsBeforePublication) {
  EXPECT_THROW(Import("1700000000.123456788 0 0 0 0 0 0 1\n"), ValidationError);
  AddFrame(session_id_, kTime1);
  EXPECT_THROW(Import(kTum), ValidationError);
  EXPECT_EQ(project_->artifacts().PayloadCount(), 0u);
  EXPECT_TRUE(project_->db().FindTrajectoriesBySession(session_id_).empty());
}

TEST_F(TrajectoryImportTest, RejectsMalformedPoseWithoutWriting) {
  const std::vector<std::string> invalid = {
      "# no poses\n",
      "1700000000.1234567891 0 0 0 0 0 0 1\n",
      "9223372036.854775808 0 0 0 0 0 0 1\n",
      "-1 0 0 0 0 0 0 1\n",
      "1700000000.123456789 nan 0 0 0 0 0 1\n",
      "1700000000.123456789 0 0 0 0 0 0 0\n",
      "1700000000.123456789 0 0 0 0 0 0 1 extra\n",
      "1700000000.124456789 0 0 0 0 0 0 1\n"
      "1700000000.123456789 0 0 0 0 0 0 1\n"};
  for (const auto& text : invalid) {
    SCOPED_TRACE(text);
    EXPECT_THROW(Import(text), ValidationError);
    EXPECT_EQ(project_->artifacts().PayloadCount(), 0u);
    EXPECT_TRUE(project_->db().FindTrajectoriesBySession(session_id_).empty());
  }
}

TEST_F(TrajectoryImportTest, ExplicitDeclarationsAndExistingCaptureAreRequired) {
  options_.source_unit.reset();
  EXPECT_THROW(Import(kTum), ValidationError);
  options_.source_unit = TrajectoryLengthUnit::kMeters;
  options_.clock_domain.reset();
  EXPECT_THROW(Import(kTum), ValidationError);
  options_.clock_domain = TrajectoryClockDomain::kPlatform;
  options_.pose_frame.reset();
  EXPECT_THROW(Import(kTum), ValidationError);
  options_.pose_frame = TrajectoryPoseFrame::kCamera;
  options_.session_id = GenerateUuid();
  EXPECT_THROW(Import(kTum), ValidationError);
  EXPECT_EQ(project_->artifacts().PayloadCount(), 0u);
}

}  // namespace
}  // namespace spatial::importers
