// COLMAP adapter real-CLI execution tests (C1-S3, RFC-0008 §9; plan §11
// test_colmap_execution). Runs the probe shim (a real child process standing
// in for a COLMAP install) through the generic subprocess runner and asserts
// the full boundary contract:
//
//   TaskRequest.input_refs -> CAS -> workspace local files -> CLI
//   workspace -> adapter discovery -> canonical Artifact -> CAS
//
// Proves: inputs never cross the CLI boundary as content hashes (argv/workspace
// assertions), the per-task workspace layout is materialized, deterministic
// progress is emitted per stage, the produced payload + provenance manifest
// round-trip through the host's fail-closed CAS ingest, and lifecycle
// failures map to typed AdapterError codes (exit, timeout, cancellation,
// missing/corrupt CAS input, missing store, unknown plan step).

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/colmap/colmap_adapter.h"
#include "adapters/colmap/colmap_cli.h"
#include "adapters/colmap/colmap_config.h"
#include "adapters/process/process_runner.h"
#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/errors/project_error.h"
#include "core/storage/metadata_db.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "engine/workers/worker_handle.h"

namespace spatial::adapters::colmap {
namespace {

using spatial::core::AdapterError;
using spatial::core::ArtifactManifest;
using spatial::core::ErrorCode;
using spatial::core::ProjectError;

#ifndef SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE
#error SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE must be defined by the test build
#endif

// Records what the adapter emits and parses manifest documents, standing in
// for the worker-protocol bridge (TaskProgress / TaskLog /
// TaskArtifactProduced).
class RecordingSink : public spatial::adapters::ResultSink {
 public:
  void Progress(int percent, const std::string& stage) override {
    stages_.push_back(stage);
    percents_.push_back(percent);
  }
  void Log(const std::string& message) override { logs_.push_back(message); }
  void ArtifactProduced(const std::string& payload_path,
                        const std::string& manifest_json) override {
    artifacts_.push_back(payload_path);
    manifests_.push_back(spatial::core::FromJsonString(manifest_json));
  }

  std::vector<std::string> stages_;
  std::vector<int> percents_;
  std::vector<std::string> logs_;
  std::vector<std::string> artifacts_;
  std::vector<ArtifactManifest> manifests_;
};

class ColmapExecutionTest : public ::testing::Test {
 protected:
  struct Input {
    std::string hash;
    std::vector<std::uint8_t> bytes;
  };

  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_colmap_ex_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = spatial::core::MetadataDb::Create(root_ / "project.db");
    store_ = std::make_unique<spatial::core::ArtifactStore>(
        root_ / "artifacts", db_);
    workspace_ = root_ / "ws";
  }

  void TearDown() override {
    store_.reset();
    db_.Close();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  static ArtifactManifest MakeManifest(const std::string& type) {
    ArtifactManifest m;
    m.type = type;
    m.producer.id = "test";
    m.producer.version = "1.0.0";
    m.producer.git_commit = "deadbeef";
    m.coordinate_frame = "scene";
    m.unit = "meter";
    m.mime_type = "application/octet-stream";
    return m;
  }

  Input PutInput(const std::string& content, const std::string& type) {
    Input in;
    in.bytes.assign(content.begin(), content.end());
    in.hash = store_->Put(in.bytes, MakeManifest(type)).content_hash;
    return in;
  }

  std::shared_ptr<ExecutionContext> MakeContext(
      const std::vector<std::string>& refs,
      const std::vector<std::string>& kinds, const std::string& config_json) {
    auto context = std::make_shared<ExecutionContext>();
    context->workspace = workspace_;
    context->store = store_.get();
    context->input_refs = refs;
    context->input_kinds = kinds;
    context->config_json = config_json;
    return context;
  }

  spatial::engine::TaskRequest RequestWith(const std::string& config_json,
                                           const std::vector<std::string>& refs) {
    spatial::engine::TaskRequest request;
    request.config_json = config_json;
    request.input_refs = refs;
    return request;
  }

  std::filesystem::path root_;
  std::filesystem::path workspace_;
  spatial::core::MetadataDb db_;
  std::unique_ptr<spatial::core::ArtifactStore> store_;
};

TEST_F(ColmapExecutionTest, EndToEndRunsStagesAndEmitsSparseModelArtifact) {
  const Input image1 = PutInput("image-one-bytes", "image");
  const Input image2 = PutInput("image-two-bytes", "image");
  const Input cal = PutInput("{\"fx\":2457.4}", "calibration");
  const std::string config_json = R"({"threads": 2})";

  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                        MakeContext({image1.hash, image2.hash, cal.hash},
                                    {"image", "image", "calibration"},
                                    config_json));
  const std::vector<std::string> plan =
      adapter.CreatePlan(RequestWith(config_json, {image1.hash}));
  EXPECT_EQ(plan, (std::vector<std::string>{
                      "feature_extractor", "matcher", "mapper"}));

  RecordingSink sink;
  adapter.Execute(plan, sink);

  EXPECT_EQ(sink.stages_, plan);
  EXPECT_EQ(sink.percents_, (std::vector<int>{33, 66, 100}));
  ASSERT_EQ(sink.artifacts_.size(), 1u);
  EXPECT_EQ(std::filesystem::path(sink.artifacts_.front()),
            workspace_ / "sparse" / "0" / "model.bin");
  EXPECT_TRUE(std::filesystem::exists(sink.artifacts_.front()));

  // Workspace layout + input materialization.
  for (const char* dir : {"images", "features", "matches", "sparse", "logs"}) {
    EXPECT_TRUE(std::filesystem::is_directory(workspace_ / dir));
  }
  EXPECT_TRUE(std::filesystem::exists(workspace_ / "database.db"));
  EXPECT_EQ(spatial::core::fs::ReadFile(workspace_ / "images" / image1.hash),
            image1.bytes);
  EXPECT_EQ(spatial::core::fs::ReadFile(workspace_ / "inputs" / image1.hash),
            image1.bytes);
  EXPECT_EQ(spatial::core::fs::ReadText(workspace_ / "calibration.json"),
            "{\"fx\":2457.4}");

  // Provenance manifest.
  ASSERT_EQ(sink.manifests_.size(), 1u);
  const ArtifactManifest& m = sink.manifests_.front();
  EXPECT_EQ(m.type, "sparse_model");
  EXPECT_EQ(m.schema_version, 1);
  EXPECT_EQ(m.producer.id, "colmap");
  EXPECT_EQ(m.producer.version, kColmapAdapterVersion);
  EXPECT_EQ(m.producer.git_commit, kColmapAdapterGitCommit);
  std::vector<std::string> expected_inputs = {image1.hash, image2.hash,
                                              cal.hash};
  std::sort(expected_inputs.begin(), expected_inputs.end());
  EXPECT_EQ(m.input_artifact_hashes, expected_inputs);
  EXPECT_EQ(m.configuration_hash, spatial::core::Sha256Hex(config_json));
  EXPECT_EQ(m.coordinate_frame, "world");
  EXPECT_EQ(m.unit, "meter");
  EXPECT_EQ(m.validation_status, "valid");

  const std::vector<std::uint8_t> payload_bytes =
      spatial::core::fs::ReadFile(sink.artifacts_.front());
  EXPECT_EQ(m.content_hash, spatial::core::Sha256Hex(payload_bytes));
  EXPECT_EQ(m.file_size, static_cast<std::int64_t>(payload_bytes.size()));

  // Host fail-closed ingest round-trip: the exact manifest document the
  // adapter emitted re-parses and registers in the CAS.
  const auto ingest = store_->Put(payload_bytes, m);
  EXPECT_TRUE(store_->Has(ingest.content_hash));
  EXPECT_EQ(store_->Get(ingest.content_hash), payload_bytes);
}

TEST_F(ColmapExecutionTest, CommandLinesUseLocalPathsNeverCasHashes) {
  const Input image1 = PutInput("img-bytes-a", "image");
  const Input image2 = PutInput("img-bytes-b", "image");

  ColmapAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
      MakeContext({image1.hash, image2.hash}, {"image", "image"}, ""));
  RecordingSink sink;
  adapter.Execute({"feature_extractor", "matcher", "mapper"}, sink);

  const std::string dump =
      spatial::core::fs::ReadText(workspace_ / "logs" / "args.txt");
  EXPECT_NE(dump.find(workspace_.string()), std::string::npos)
      << "the CLI must receive the local workspace path";
  EXPECT_EQ(dump.find(image1.hash), std::string::npos)
      << "a CAS content hash must never appear in the CLI argv";
  EXPECT_EQ(dump.find(image2.hash), std::string::npos)
      << "a CAS content hash must never appear in the CLI argv";
}

TEST_F(ColmapExecutionTest, NonZeroExitIsDeterministicFailure) {
  std::filesystem::create_directories(workspace_);
  std::ofstream(workspace_ / "shim_fail").close();
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, MakeContext({}, {}, ""));
  RecordingSink sink;

  try {
    adapter.Execute({"feature_extractor"}, sink);
    FAIL() << "expected AdapterError for a non-zero CLI exit";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
    EXPECT_FALSE(e.recoverable());
    EXPECT_NE(e.message().find("with code 42"), std::string::npos);
    EXPECT_TRUE(sink.artifacts_.empty());
  }
}

TEST_F(ColmapExecutionTest, TimeoutIsTransientFailureAndTerminatesChild) {
  std::filesystem::create_directories(workspace_);
  std::ofstream(workspace_ / "shim_hang").close();
  auto context = MakeContext({}, {}, "");
  context->stage_timeout_ms = 500;
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, context);
  RecordingSink sink;

  const auto start = std::chrono::steady_clock::now();
  try {
    adapter.Execute({"feature_extractor"}, sink);
    FAIL() << "expected AdapterError for a timed-out CLI tool";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessTimeout);
    EXPECT_TRUE(e.recoverable());
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_LT(elapsed.count(), 5000) << "the hung child must be terminated";
  EXPECT_TRUE(sink.artifacts_.empty());
}

TEST_F(ColmapExecutionTest, PreCancelledTokenStopsBeforeSpawning) {
  spatial::adapters::process::CancelToken token;
  token.Cancel();
  auto context = MakeContext({}, {}, "");
  context->cancel = &token;
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, context);
  RecordingSink sink;

  try {
    adapter.Execute({"feature_extractor"}, sink);
    FAIL() << "expected AdapterError for a cancelled task";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessCancelled);
    EXPECT_FALSE(e.recoverable());
  }
  EXPECT_FALSE(spatial::core::fs::Exists(workspace_ / "logs" / "args.txt"))
      << "no stage may have spawned after cancellation";
}

TEST_F(ColmapExecutionTest, MissingInputInCasFailsClosed) {
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                        MakeContext({"deadbeef0123"}, {"image"}, ""));
  RecordingSink sink;

  try {
    adapter.Execute({"feature_extractor"}, sink);
    FAIL() << "expected AdapterError for an input missing from the CAS";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
    EXPECT_NE(e.message().find("not in CAS"), std::string::npos);
  }
  EXPECT_FALSE(spatial::core::fs::Exists(workspace_ / "logs" / "args.txt"));
}

TEST_F(ColmapExecutionTest, CorruptInputInCasFailsClosed) {
  const Input image = PutInput("pristine-bytes", "image");
  const std::filesystem::path cas_payload =
      root_ / "artifacts" / "cas" /
      image.hash.substr(0, 2) / image.hash;
  std::ofstream(cas_payload) << "corrupted";

  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                        MakeContext({image.hash}, {"image"}, ""));
  RecordingSink sink;

  try {
    adapter.Execute({"feature_extractor"}, sink);
    FAIL() << "expected AdapterError for a corrupt CAS input";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
    EXPECT_NE(e.message().find("corrupt in CAS"), std::string::npos);
  }
  EXPECT_FALSE(spatial::core::fs::Exists(workspace_ / "logs" / "args.txt"));
}

TEST_F(ColmapExecutionTest, MissingStoreWithInputsFailsClosed) {
  auto context = std::make_shared<ExecutionContext>();
  context->workspace = workspace_;
  context->store = nullptr;
  context->input_refs = {"deadbeef0123"};
  context->input_kinds = {"image"};
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, context);
  RecordingSink sink;

  try {
    adapter.Execute({"feature_extractor"}, sink);
    FAIL() << "expected AdapterError without a bound artifact store";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
    EXPECT_NE(e.message().find("requires an artifact store"),
              std::string::npos);
  }
}

TEST_F(ColmapExecutionTest, UnknownInputKindIsRejected) {
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                        MakeContext({"deadbeef0123"}, {"mesh"}, ""));
  RecordingSink sink;

  try {
    adapter.Execute({"feature_extractor"}, sink);
    FAIL() << "expected AdapterError for an undeclared input kind";
  } catch (const AdapterError& e) {
    EXPECT_EQ(e.code(), ErrorCode::kAdapterProcessFailed);
    EXPECT_NE(e.message().find("unknown input kind"), std::string::npos);
  }
}

TEST_F(ColmapExecutionTest, EmptyPlanIsRejectedWithoutSubprocess) {
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, MakeContext({}, {}, ""));
  RecordingSink sink;
  EXPECT_THROW(adapter.Execute({}, sink), ProjectError);
  EXPECT_FALSE(spatial::core::fs::Exists(workspace_ / "logs" / "args.txt"));
}

TEST_F(ColmapExecutionTest, UnknownPlanStepIsRejected) {
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, MakeContext({}, {}, ""));
  RecordingSink sink;
  EXPECT_THROW(adapter.Execute({"feature_extractor", "meshing"}, sink),
               ProjectError);
}

}  // namespace
}  // namespace spatial::adapters::colmap
