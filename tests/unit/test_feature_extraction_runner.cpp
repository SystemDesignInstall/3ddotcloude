// Mock feature_extract runner tests (RFC-0007 §6, P2.3 C3). The feature_extract
// task branch genuinely reads the input image bytes and derives a deterministic
// FeatureArtifact (feature.schema.json) behind the worker boundary, via the
// scene-agnostic payload writer (ADR-038). Provenance is a hard contract: the
// output manifest's input_artifact_hashes[0] must equal the input image hash.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
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
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"
#include "engine/workers/mock_pipeline_runner.h"
#include "schema_check.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactStore;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;
using spatial::core::Sha256Hex;
using spatial::core::Uuid;
using nlohmann::json;

const Uuid kProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

class FeatureExtractionRunnerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_featrun_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = MetadataDb::Create(root_ / "project.db");
    db_->InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978", "local",
                       "{}", "{}");
    store_ = std::make_unique<ArtifactStore>(root_ / "artifacts", *db_);
    runner_ = MakeMockPipelineRunner(*store_);
  }

  void TearDown() override {
    runner_ = nullptr;
    store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Puts the input image bytes into the CAS; returns the content hash that the
  // task input ref must reference.
  std::string PutImage(const std::string& content) {
    const std::vector<std::uint8_t> bytes(content.begin(), content.end());
    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = "image";
    manifest.producer = {"spatial-platform", "0.1.0", "test"};
    manifest.creation_timestamp = "2026-01-01T00:00:00Z";
    manifest.file_size = static_cast<std::int64_t>(bytes.size());
    return store_->Put(bytes, manifest).content_hash;
  }

  TaskRequest MakeRequest(const std::vector<std::string>& input_refs,
                          const std::string& config_json) {
    TaskRequest request;
    request.task_id = GenerateUuid();
    request.task_type = "feature_extract";
    request.input_refs = input_refs;
    request.config_json = config_json;
    return request;
  }

  // Runs the task to completion and returns the emitted events.
  std::vector<WorkerEvent> Run(const TaskRequest& request) {
    std::vector<WorkerEvent> events;
    runner_(request,
            [&events](WorkerEvent event) { events.push_back(event); },
            []() { return false; });
    return events;
  }

  std::string ProducedRef(const std::vector<WorkerEvent>& events) const {
    for (const auto& event : events) {
      if (event.type == WorkerEventType::kArtifactProduced) {
        return event.artifact_ref;
      }
    }
    return {};
  }

  json LoadFeatureSchema() const {
    std::ifstream in(SPATIAL_FEATURE_SCHEMA_JSON);
    EXPECT_TRUE(in.good()) << "cannot open feature.schema.json";
    return json::parse(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }

  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> store_;
  InProcessTaskRunner runner_;
};

TEST_F(FeatureExtractionRunnerTest, ProducesSchemaValidFeatureArtifact) {
  const std::string input = PutImage("image-a");
  const auto events = Run(MakeRequest({input}, "{}"));

  // The task completes and reports exactly one produced artifact.
  ASSERT_TRUE(events.back().type == WorkerEventType::kCompleted);
  const std::string ref = ProducedRef(events);
  ASSERT_FALSE(ref.empty());

  // The produced payload is the FeatureArtifact document feature.schema.json
  // describes (RFC-0007 §2), and the CAS hash matches the produced ref.
  const auto bytes = store_->Get(ref);
  ASSERT_TRUE(bytes.has_value());
  const auto payload = json::parse(std::string(bytes->begin(), bytes->end()));

  std::vector<std::string> violations;
  CheckNode(LoadFeatureSchema(), payload, "$", &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();

  // Producer guarantee: count == keypoints.length == descriptors.length, with
  // the default 64-keypoint mock grid.
  const auto count = payload["count"].get<std::int64_t>();
  EXPECT_EQ(count, static_cast<std::int64_t>(payload["keypoints"].size()));
  EXPECT_EQ(count, static_cast<std::int64_t>(payload["descriptors"].size()));
  EXPECT_EQ(count, 64);
  EXPECT_EQ(payload["schema_version"], 1);
  EXPECT_EQ(payload["detector"], "mock");
  EXPECT_EQ(payload["descriptor_type"], "mock_16");
}

TEST_F(FeatureExtractionRunnerTest, ManifestRecordsMetadataAndProvenance) {
  const std::string input = PutImage("image-a");
  const auto events = Run(MakeRequest({input}, "{}"));
  const std::string ref = ProducedRef(events);
  ASSERT_FALSE(ref.empty());

  // Resolve the manifest through the artifact index: the produced ref is the
  // content hash the manifest was written under.
  const auto indexed = db_->FindArtifactByHash(ref);
  ASSERT_TRUE(indexed.has_value());
  const auto manifest = store_->ReadManifest(indexed->artifact_id);
  ASSERT_TRUE(manifest.has_value());

  EXPECT_EQ(manifest->type, "feature");
  EXPECT_EQ(manifest->schema_version, 1);
  EXPECT_EQ(manifest->mime_type, "application/json");
  EXPECT_EQ(manifest->coordinate_frame, "image");
  EXPECT_EQ(manifest->unit, "pixels");
  EXPECT_EQ(manifest->content_hash, ref);
  EXPECT_EQ(manifest->file_size, static_cast<std::int64_t>(
                                    store_->Get(ref)->size()));

  // Provenance contract (RFC-0007 §6): the feature artifact is derived from
  // exactly the input image, and the recorded hash is the image's CAS hash.
  ASSERT_EQ(manifest->input_artifact_hashes.size(), 1u);
  EXPECT_EQ(manifest->input_artifact_hashes[0], input);

  // Provenance (P2.3 M1): the manifest records the effective stage config the
  // worker consumed; Sha256Hex("{}") is the ADR-020 cache-key config digest.
  EXPECT_EQ(manifest->configuration_hash, Sha256Hex("{}"));
}

TEST_F(FeatureExtractionRunnerTest, ConfigurationHashPersistsThroughManifest) {
  const std::string input = PutImage("image-a");

  // Run with a non-trivial config, then read the manifest straight from the
  // on-disk file artifacts/<uuid>/manifest.json (ArtifactManifest::Read), not
  // the in-memory copy, to prove the round-trip (P2.3 M1 exit criteria).
  const auto events = Run(
      MakeRequest({input}, R"({"config": {"keypoint_count": 128}})"));
  const std::string ref = ProducedRef(events);
  ASSERT_FALSE(ref.empty());
  const auto indexed = db_->FindArtifactByHash(ref);
  ASSERT_TRUE(indexed.has_value());

  const auto persisted = ArtifactManifest::Read(
      root_ / "artifacts" / FormatUuid(indexed->artifact_id) /
      "manifest.json");
  EXPECT_EQ(persisted.configuration_hash,
            Sha256Hex(R"({"config": {"keypoint_count": 128}})"));

  // A different effective config must yield a different persisted hash: the
  // recorded value reflects what actually produced the artifact.
  const auto default_events = Run(MakeRequest({input}, "{}"));
  const auto default_indexed =
      db_->FindArtifactByHash(ProducedRef(default_events));
  ASSERT_TRUE(default_indexed.has_value());
  const auto default_persisted = ArtifactManifest::Read(
      root_ / "artifacts" / FormatUuid(default_indexed->artifact_id) /
      "manifest.json");
  EXPECT_EQ(default_persisted.configuration_hash, Sha256Hex("{}"));
  EXPECT_NE(persisted.configuration_hash,
            default_persisted.configuration_hash);
}

TEST_F(FeatureExtractionRunnerTest, DeterministicAcrossRunsAndInputs) {
  const std::string input = PutImage("image-a");

  const auto first = Run(MakeRequest({input}, "{}"));
  const auto second = Run(MakeRequest({input}, "{}"));
  const std::string first_ref = ProducedRef(first);
  const std::string second_ref = ProducedRef(second);
  ASSERT_FALSE(first_ref.empty());
  ASSERT_FALSE(second_ref.empty());

  // AC-8: identical image bytes -> identical feature bytes -> identical hash.
  EXPECT_EQ(second_ref, first_ref);
  const auto first_bytes = store_->Get(first_ref);
  const auto second_bytes = store_->Get(second_ref);
  ASSERT_TRUE(first_bytes.has_value());
  ASSERT_TRUE(second_bytes.has_value());
  EXPECT_EQ(*second_bytes, *first_bytes);

  // Different image bytes -> a different (deterministic) feature payload.
  const std::string other_input = PutImage("image-b");
  const auto third = Run(MakeRequest({other_input}, "{}"));
  const std::string third_ref = ProducedRef(third);
  ASSERT_FALSE(third_ref.empty());
  EXPECT_NE(third_ref, first_ref);
}

TEST_F(FeatureExtractionRunnerTest, KeypointCountHonorsConfig) {
  const std::string input = PutImage("image-a");

  const auto default_run = Run(MakeRequest({input}, "{}"));
  const auto sized_run =
      Run(MakeRequest({input}, R"({"config": {"keypoint_count": 128}})"));
  ASSERT_FALSE(ProducedRef(default_run).empty());
  ASSERT_FALSE(ProducedRef(sized_run).empty());

  const auto default_bytes = store_->Get(ProducedRef(default_run));
  const auto sized_bytes = store_->Get(ProducedRef(sized_run));
  ASSERT_TRUE(default_bytes.has_value());
  ASSERT_TRUE(sized_bytes.has_value());

  const auto default_payload =
      json::parse(std::string(default_bytes->begin(), default_bytes->end()));
  const auto sized_payload =
      json::parse(std::string(sized_bytes->begin(), sized_bytes->end()));

  const auto default_count = default_payload["count"].get<std::int64_t>();
  const auto sized_count = sized_payload["count"].get<std::int64_t>();
  EXPECT_EQ(default_count, 64);
  EXPECT_EQ(sized_count, 128);
  EXPECT_EQ(default_count,
            static_cast<std::int64_t>(default_payload["keypoints"].size()));
  EXPECT_EQ(sized_count,
            static_cast<std::int64_t>(sized_payload["keypoints"].size()));
  EXPECT_EQ(sized_count,
            static_cast<std::int64_t>(sized_payload["descriptors"].size()));

  // Config changes the derived feature payload, hence a different hash.
  EXPECT_NE(ProducedRef(sized_run), ProducedRef(default_run));
}

TEST_F(FeatureExtractionRunnerTest, MissingInputImageThrows) {
  // No input ref at all is a domain error, not a crash.
  EXPECT_THROW(Run(MakeRequest({}, "{}")), spatial::core::ProjectError);

  // A ref that was never stored is equally rejected.
  EXPECT_THROW(Run(MakeRequest({"absent-image-hash"}, "{}")),
               spatial::core::ProjectError);

  // Neither path produced anything.
  EXPECT_EQ(db_->FindArtifactsByType("feature").size(), 0u);
}

}  // namespace
}  // namespace spatial::engine
