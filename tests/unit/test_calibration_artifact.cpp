#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_store.h"
#include "core/artifacts/calibration_artifact.h"
#include "core/errors/project_error.h"
#include "core/scene/query/calibration_materializer.h"
#include "core/scene/query/scene_query.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"
#include "schema_check.h"

namespace spatial::core {
namespace {

using nlohmann::json;

const Uuid kProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

const ProducerInfo kSessionProducer = {"session", "0.1.0", "abc1234"};

WriteCalibrationArtifactInput MakeInput() {
  WriteCalibrationArtifactInput input;
  input.calibration_id = FormatUuid(GenerateUuid());
  input.sensor_id = FormatUuid(GenerateUuid());
  input.version = 2;
  input.calibration_time_ns = 1783123200000000000LL;
  input.source = "factory";
  input.intrinsic_model = "opencv";
  input.intrinsics_json =
      R"({"fx":2457.4,"fy":2456.9,"cx":2000.0,"cy":1500.0})";
  input.distortion_json = R"({"model":"opencv_radial","coefficients":[-0.12,0.03,0.0,0.0]})";
  input.extrinsics_json =
      R"({"from_frame":"camera_1","to_frame":"sensor_rig_0","rot_xyzw":[0.0,0.0,0.0,1.0],"t_xyz":[0.0,0.0,0.0]})";
  input.uncertainty_json = R"({"fx":1.2,"fy":1.2})";
  input.valid_from_ns = 100;
  input.valid_to_ns = 900;
  input.input_artifact_hashes = {};
  input.configuration_hash = "calibration-config-hash-fixture";
  input.coordinate_frame = "sensor_rig_0";
  input.producer = kSessionProducer;
  return input;
}

class CalibrationArtifactTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_cal_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = MetadataDb::Create(root_ / "project.db");
    db_->InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978",
                       "local", "{}", "{}");
    store_ = std::make_unique<ArtifactStore>(root_ / "artifacts", *db_);
  }

  void TearDown() override {
    store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  json LoadCalibrationSchema() const {
    std::ifstream in(SPATIAL_CALIBRATION_SCHEMA_JSON);
    EXPECT_TRUE(in.good()) << "cannot open calibration.schema.json";
    return json::parse(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }

  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> store_;
};

TEST_F(CalibrationArtifactTest, ProducedPayloadValidatesAgainstCalibrationSchema) {
  const auto result = WriteCalibrationArtifact(*store_, MakeInput());

  const auto bytes = store_->Get(result.content_hash);
  ASSERT_TRUE(bytes.has_value());
  const auto payload = json::parse(std::string(bytes->begin(), bytes->end()));

  std::vector<std::string> violations;
  CheckNode(LoadCalibrationSchema(), payload, "$", &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();

  EXPECT_EQ(payload["version"], 2);
  EXPECT_EQ(payload["intrinsic_model"], "opencv");
  EXPECT_EQ(payload["intrinsics"]["fx"], 2457.4);
  EXPECT_EQ(payload["distortion"]["model"], "opencv_radial");
  EXPECT_EQ(payload["valid_from_ns"], 100);
  EXPECT_EQ(payload["valid_to_ns"], 900);
}

TEST_F(CalibrationArtifactTest, FovIntrinsicModelValidatesAgainstSchema) {
  auto input = MakeInput();
  input.intrinsic_model = "fov";
  input.intrinsics_json =
      R"({"fx":4000.0,"fy":4000.0,"cx":2000.0,"cy":1500.0,"omega":0.35})";

  const auto result = WriteCalibrationArtifact(*store_, input);
  const auto bytes = store_->Get(result.content_hash);
  ASSERT_TRUE(bytes.has_value());
  const auto payload = json::parse(std::string(bytes->begin(), bytes->end()));

  std::vector<std::string> violations;
  CheckNode(LoadCalibrationSchema(), payload, "$", &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();
  EXPECT_EQ(payload["intrinsic_model"], "fov");
}

TEST_F(CalibrationArtifactTest, ManifestRecordsCalibrationMetadata) {
  const auto result = WriteCalibrationArtifact(*store_, MakeInput());

  const auto manifest = store_->ReadManifest(result.artifact_uuid);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->type, "calibration");
  EXPECT_EQ(manifest->schema_version, 1);
  EXPECT_EQ(manifest->mime_type, "application/json");
  EXPECT_EQ(manifest->unit, "meter");
  EXPECT_EQ(manifest->coordinate_frame, "sensor_rig_0");
  EXPECT_EQ(manifest->configuration_hash, "calibration-config-hash-fixture");
  EXPECT_EQ(manifest->producer.id, "session");
  EXPECT_EQ(manifest->content_hash, result.content_hash);
}

TEST_F(CalibrationArtifactTest, DeterministicAndDeduplicates) {
  const auto input = MakeInput();
  const auto first = WriteCalibrationArtifact(*store_, input);
  const auto second = WriteCalibrationArtifact(*store_, input);
  EXPECT_EQ(second.content_hash, first.content_hash);
  EXPECT_TRUE(second.deduplicated);
  EXPECT_EQ(store_->PayloadCount(), 1);
}

TEST_F(CalibrationArtifactTest, RejectsMalformedDeclaredJson) {
  auto input = MakeInput();
  input.intrinsics_json = "{not-json";
  EXPECT_THROW(WriteCalibrationArtifact(*store_, input),
               ProjectError);
  EXPECT_EQ(db_->FindArtifactsByType("calibration").size(), 0u);
}

TEST_F(CalibrationArtifactTest, MaterializesFromResolvedSceneRecord) {
  const Uuid sensor_id = GenerateUuid();
  SensorRow sensor;
  sensor.sensor_id = sensor_id;
  sensor.project_id = kProjectId;
  sensor.type = "camera";
  sensor.model = "fixture-camera";
  db_->RegisterSensor(sensor);

  const auto calibration_id = GenerateUuid();
  CalibrationRow row;
  row.calibration_id = calibration_id;
  row.sensor_id = sensor_id;
  row.version = 1;
  row.calibration_time_ns = 500;
  row.source = "factory";
  row.intrinsics_json = R"({"fx":2457.4,"fy":2456.9,"cx":2000.0,"cy":1500.0})";
  row.valid_from_ns = 100;
  row.valid_to_ns = 900;
  db_->AddCalibration(row);

  const SceneQuery query(*db_);
  const auto resolved = query.ResolveCalibrationAt(sensor_id, TimestampNs(500));
  ASSERT_TRUE(resolved.has_value());

  MaterializeCalibrationOptions options;
  options.producer = kSessionProducer;
  options.coordinate_frame = "sensor_rig_0";
  const auto result = MaterializeCalibrationArtifact(*store_, *resolved, options);

  const auto bytes = store_->Get(result.content_hash);
  ASSERT_TRUE(bytes.has_value());
  const auto payload = json::parse(std::string(bytes->begin(), bytes->end()));

  std::vector<std::string> violations;
  CheckNode(LoadCalibrationSchema(), payload, "$", &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();

  EXPECT_EQ(payload["calibration_id"], FormatUuid(calibration_id));
  EXPECT_EQ(payload["sensor_id"], FormatUuid(sensor_id));
  EXPECT_EQ(payload["version"], 1);
  EXPECT_EQ(payload["source"], "factory");
  EXPECT_EQ(payload["intrinsics"]["fx"], 2457.4);
  EXPECT_EQ(payload["valid_from_ns"], 100);
  EXPECT_EQ(payload["valid_to_ns"], 900);

  // The artifact is immutable and content-addressed; identical records dedupe.
  const auto again = MaterializeCalibrationArtifact(*store_, *resolved, options);
  EXPECT_EQ(again.content_hash, result.content_hash);
  EXPECT_TRUE(again.deduplicated);
}

}  // namespace
}  // namespace spatial::core
