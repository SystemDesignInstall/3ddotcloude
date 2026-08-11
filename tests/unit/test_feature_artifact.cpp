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
#include "core/errors/project_error.h"
#include "core/scene/identity.h"
#include "core/storage/metadata_db.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/feature_extraction.h"
#include "schema_check.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactStore;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;
using spatial::core::Uuid;
using nlohmann::json;

const Uuid kProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

void InsertTestProject(MetadataDb& db) {
  db.InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978", "local",
                   "{}", "{}");
}

// Canonical M0 fixture (RFC-0007 §2): detector "mock", descriptor_type
// "mock_16" (16-dim float rows). Three keypoints, three descriptor rows.
WriteFeatureArtifactInput MakeInput(const Uuid& frame_id) {
  WriteFeatureArtifactInput input;
  input.frame_id = frame_id;
  input.detector = "mock";
  input.descriptor_type = "mock_16";
  input.input_content_hash = "image-bytes-hash-fixture";
  input.keypoints = {
      {10.0, 20.0, 4.0, 0.1, 0.9},
      {30.0, 40.0, 4.5, 0.2, 0.8},
      {50.0, 60.0, 5.0, 0.3, 0.7},
  };
  for (std::size_t i = 0; i < input.keypoints.size(); ++i) {
    std::vector<double> row;
    for (int d = 0; d < 16; ++d) {
      row.push_back(static_cast<double>(i * 16 + d) / 255.0);
    }
    input.descriptors.push_back(std::move(row));
  }
  return input;
}

class FeatureArtifactTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_feat_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = MetadataDb::Create(root_ / "project.db");
    InsertTestProject(*db_);
    store_ = std::make_unique<ArtifactStore>(root_ / "artifacts", *db_);
  }

  void TearDown() override {
    store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Reads the feature.schema.json from the compile-time path.
  json LoadFeatureSchema() const {
    std::ifstream in(SPATIAL_FEATURE_SCHEMA_JSON);
    EXPECT_TRUE(in.good()) << "cannot open feature.schema.json";
    return json::parse(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }

  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> store_;
};

TEST_F(FeatureArtifactTest, ProducedPayloadValidatesAgainstFeatureSchema) {
  const Uuid frame_id = GenerateUuid();
  const auto result =
      WriteFeatureArtifact(*store_, *db_, MakeInput(frame_id));

  // The FeatureArtifact payload (the bytes in the CAS) is the JSON document
  // that feature.schema.json describes (RFC-0007 §2); validate exactly what
  // the writer stored.
  const auto bytes = store_->Get(result.content_hash);
  ASSERT_TRUE(bytes.has_value());
  const auto payload = json::parse(std::string(bytes->begin(), bytes->end()));

  std::vector<std::string> violations;
  CheckNode(LoadFeatureSchema(), payload, "$", &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();

  // Producer guarantee (RFC-0007 §2): count == keypoints.length ==
  // descriptors.length.
  EXPECT_EQ(payload["count"].get<std::int64_t>(),
            static_cast<std::int64_t>(payload["keypoints"].size()));
  EXPECT_EQ(payload["count"].get<std::int64_t>(),
            static_cast<std::int64_t>(payload["descriptors"].size()));
  EXPECT_EQ(payload["count"], 3);
  EXPECT_EQ(payload["schema_version"], 1);
}

TEST_F(FeatureArtifactTest, ManifestRecordsFeatureMetadata) {
  const Uuid frame_id = GenerateUuid();
  const auto result =
      WriteFeatureArtifact(*store_, *db_, MakeInput(frame_id));

  const auto manifest = store_->ReadManifest(result.artifact_uuid);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->type, "feature");
  EXPECT_EQ(manifest->schema_version, 1);
  EXPECT_EQ(manifest->mime_type, "application/json");
  EXPECT_EQ(manifest->coordinate_frame, "image");
  EXPECT_EQ(manifest->unit, "pixels");
  ASSERT_EQ(manifest->input_artifact_hashes.size(), 1u);
  EXPECT_EQ(manifest->input_artifact_hashes[0], "image-bytes-hash-fixture");
  EXPECT_EQ(manifest->content_hash, result.content_hash);
}

TEST_F(FeatureArtifactTest, RecordsFeatureSetRowAndReadsBack) {
  const auto scene = db_->FindOrCreateScene(kProjectId, "scene", "{}", 1000);
  const Uuid frame_id = GenerateUuid();

  spatial::core::FrameRow frame;
  frame.frame_id = frame_id;
  frame.scene_id = scene.scene_id;
  db_->InsertFrame(frame);

  const auto result =
      WriteFeatureArtifact(*store_, *db_, MakeInput(frame_id));

  const auto by_frame = db_->FindFeatureSetsByFrame(frame_id);
  ASSERT_EQ(by_frame.size(), 1u);
  EXPECT_EQ(by_frame[0].feature_set_id, result.feature_set.feature_set_id);
  EXPECT_EQ(by_frame[0].frame_id, frame_id);
  EXPECT_EQ(by_frame[0].detector, "mock");
  EXPECT_EQ(by_frame[0].descriptor_type, "mock_16");
  EXPECT_EQ(by_frame[0].count, 3);
  EXPECT_EQ(by_frame[0].artifact_ref, FormatUuid(result.artifact_uuid));

  // Scene read resolves through the frames join.
  const auto by_scene = db_->FindFeatureSetsByScene(scene.scene_id);
  ASSERT_EQ(by_scene.size(), 1u);
  EXPECT_EQ(by_scene[0].feature_set_id, result.feature_set.feature_set_id);

  // A frame outside the scene is not visible through the scene read.
  const Uuid kOtherProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
  db_->InsertProject(kOtherProjectId, "other", 1, "{}", 1000, "EPSG:4978",
                     "local", "{}", "{}");
  const auto other_scene =
      db_->FindOrCreateScene(kOtherProjectId, "other", "{}", 1000);
  const Uuid other_frame = GenerateUuid();
  spatial::core::FrameRow other;
  other.frame_id = other_frame;
  other.scene_id = other_scene.scene_id;
  db_->InsertFrame(other);
  const auto other_result =
      WriteFeatureArtifact(*store_, *db_, MakeInput(other_frame));
  EXPECT_EQ(db_->FindFeatureSetsByScene(scene.scene_id).size(), 1u);
  EXPECT_EQ(db_->FindFeatureSetsByFrame(other_frame).size(), 1u);
  EXPECT_NE(other_result.feature_set.feature_set_id,
            result.feature_set.feature_set_id);
}

TEST_F(FeatureArtifactTest, DeterministicAndIdempotent) {
  const Uuid frame_id = GenerateUuid();
  const auto first = WriteFeatureArtifact(*store_, *db_, MakeInput(frame_id));

  // Identical inputs -> identical bytes (AC-8) and the same derived id; the
  // re-run deduplicates and must not violate the feature_sets PK.
  const auto second = WriteFeatureArtifact(*store_, *db_, MakeInput(frame_id));
  EXPECT_EQ(second.content_hash, first.content_hash);
  EXPECT_EQ(second.feature_set.feature_set_id,
            first.feature_set.feature_set_id);
  EXPECT_TRUE(second.deduplicated);

  const auto by_frame = db_->FindFeatureSetsByFrame(frame_id);
  ASSERT_EQ(by_frame.size(), 1u);
  EXPECT_EQ(by_frame[0].feature_set_id, first.feature_set.feature_set_id);
}

TEST_F(FeatureArtifactTest, RejectsKeypointsDescriptorsMismatch) {
  const Uuid frame_id = GenerateUuid();
  auto input = MakeInput(frame_id);
  input.keypoints.pop_back();  // 2 keypoints, 3 descriptor rows
  EXPECT_THROW(WriteFeatureArtifact(*store_, *db_, input),
               spatial::core::ProjectError);
  EXPECT_EQ(db_->FindFeatureSetsByFrame(frame_id).size(), 0u);
  EXPECT_EQ(db_->FindArtifactsByType("feature").size(), 0u);
}

}  // namespace
}  // namespace spatial::engine
