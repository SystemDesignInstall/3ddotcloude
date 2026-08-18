// COLMAP worker protocol tests (C1-S4; plan §11 test_colmap_worker). Drives
// the real spatial_colmap_worker binary through the engine's ProcessExecutor
// (the same seam the scheduler uses): WorkerHello handshake, TaskRequest ->
// TaskAccepted -> TaskProgress(substages) -> TaskArtifactProduced ->
// TaskCompleted, fail-closed CAS ingest of the produced sparse_model payload,
// shim-failure marker -> TaskFailed with the stable ADAPTER_PROCESS_FAILED
// code, and cooperative cancellation of a hung stage.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/storage/metadata_db.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/workers/process_executor.h"
#include "engine/workers/worker_handle.h"
#include "tests/unit/engine_test_helpers.h"

namespace spatial::engine {
namespace {

#ifndef SPATIAL_COLMAP_WORKER_EXECUTABLE
#error SPATIAL_COLMAP_WORKER_EXECUTABLE must be defined by the test build
#endif
#ifndef SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE
#error SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE must be defined by the test build
#endif

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactStore;
using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;
using spatial::core::Uuid;

std::vector<WorkerEvent> DrainToTerminal(ProcessExecutor& executor,
                                         std::int64_t timeout_events_ms) {
  std::vector<WorkerEvent> events;
  WorkerEvent event;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (executor.WaitForEvent(event, timeout_events_ms)) {
      events.push_back(event);
      if (event.type == WorkerEventType::kCompleted ||
          event.type == WorkerEventType::kFailed ||
          event.type == WorkerEventType::kCancelled) {
        break;
      }
    }
  }
  return events;
}

class ColmapWorkerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_colmap_worker_" + std::to_string(std::time(nullptr)) +
             "_" + std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = MetadataDb::Create(root_ / "project.db");
    store_ = std::make_unique<ArtifactStore>(root_ / "artifacts", db_);
  }

  void TearDown() override {
    store_.reset();
    db_.Close();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::unique_ptr<ProcessExecutor> MakeExecutor(ArtifactStore* store) {
    return std::make_unique<ProcessExecutor>(
        test::BigWorker(),
        std::vector<std::string>{SPATIAL_COLMAP_WORKER_EXECUTABLE,
                                 SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE},
        "", 5000, store);
  }

  ArtifactRef PutImage(const std::string& content) {
    ArtifactManifest manifest;
    manifest.type = "image";
    manifest.producer.id = "test";
    manifest.producer.version = "1.0.0";
    manifest.mime_type = "application/octet-stream";
    const std::vector<std::uint8_t> bytes(content.begin(), content.end());
    return store_->Put(bytes, manifest).content_hash;
  }

  std::filesystem::path root_;
  MetadataDb db_;
  std::unique_ptr<ArtifactStore> store_;
};

TEST_F(ColmapWorkerTest, HandshakeNegotiatesColmapCapabilities) {
  auto executor = MakeExecutor(nullptr);
  const auto& profile = executor->profile();
  EXPECT_NE(profile.capabilities.end(),
            std::find(profile.capabilities.begin(),
                      profile.capabilities.end(), "sparse_reconstruction"));
  EXPECT_NE(profile.capabilities.end(),
            std::find(profile.capabilities.begin(),
                      profile.capabilities.end(), "bundle_adjustment"));
  EXPECT_GE(profile.max_concurrency, 1);
  EXPECT_FALSE(spatial::core::IsNil(executor->id()));
}

TEST_F(ColmapWorkerTest, EndToEndProducesCasSparseModelArtifact) {
  const ArtifactRef image1 = PutImage("image-one-bytes");
  const ArtifactRef image2 = PutImage("image-two-bytes");

  TaskRequest request;
  request.task_id = GenerateUuid();
  request.task_type = "sparse_reconstruction";
  request.config_json = R"({"threads": 1})";
  request.input_refs = {image1, image2};
  request.workspace = (root_ / "ws").string();

  auto executor = MakeExecutor(store_.get());
  executor->Submit(request);

  const auto events = DrainToTerminal(*executor, 2000);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().type, WorkerEventType::kCompleted);

  bool saw_progress = false;
  bool saw_artifact = false;
  for (const auto& e : events) {
    if (e.type == WorkerEventType::kProgress) {
      saw_progress = true;
    }
    if (e.type == WorkerEventType::kArtifactProduced) {
      saw_artifact = true;
      EXPECT_EQ(e.artifact_ref.size(), 64u);
      EXPECT_FALSE(e.payload_path.empty());
      EXPECT_FALSE(e.manifest_json.empty());
    }
  }
  EXPECT_TRUE(saw_progress);
  ASSERT_TRUE(saw_artifact);

  // Host materialization + worker staging: inputs reached the CLI layout.
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(request.workspace) /
                                      "inputs" / image1));
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(request.workspace) /
                                      "images" / image1));
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(request.workspace) /
                                      "images" / image2));

  // Fail-closed CAS ingest: the produced payload is the canonical Reconstruction v2.
  EXPECT_TRUE(store_->Has(events.back().artifact_ref));
  const auto payload = store_->Get(events.back().artifact_ref);
  ASSERT_TRUE(payload.has_value());
  const nlohmann::json document = nlohmann::json::parse(
      std::string(payload->begin(), payload->end()));
  EXPECT_EQ(document["schema_version"].get<int>(), 2);
  EXPECT_TRUE(document.contains("reconstruction_id"));
  ASSERT_EQ(document["cameras"].size(), 1u);
  EXPECT_EQ(document["cameras"][0]["intrinsic_model"].get<std::string>(),
            "pinhole");
  EXPECT_EQ(document["images"].size(), 2u);
  EXPECT_EQ(document["points3D"].size(), 1u);
}

TEST_F(ColmapWorkerTest, ShimFailureMarkerSendsTaskFailed) {
  const std::filesystem::path ws = root_ / "ws";
  std::filesystem::create_directories(ws);
  std::ofstream(ws / "shim_fail").close();

  TaskRequest request;
  request.task_id = GenerateUuid();
  request.task_type = "sparse_reconstruction";
  request.config_json = "{}";
  request.workspace = ws.string();

  auto executor = MakeExecutor(store_.get());
  executor->Submit(request);

  const auto events = DrainToTerminal(*executor, 2000);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().type, WorkerEventType::kFailed);
  EXPECT_EQ(events.back().error_code, "ADAPTER_PROCESS_FAILED");
  EXPECT_FALSE(events.back().recoverable);
  EXPECT_FALSE(events.back().error_message.empty());
  EXPECT_EQ(store_->PayloadCount(), 0u);
}

TEST_F(ColmapWorkerTest, CancelStopsHungStageAndReportsCancelled) {
  const std::filesystem::path ws = root_ / "ws";
  std::filesystem::create_directories(ws);
  std::ofstream(ws / "shim_hang").close();

  TaskRequest request;
  request.task_id = GenerateUuid();
  request.task_type = "sparse_reconstruction";
  request.config_json = "{}";
  request.workspace = ws.string();

  auto executor = MakeExecutor(store_.get());
  executor->Submit(request);

  // Wait until the worker is alive and has accepted the task (heartbeats),
  // then cancel the hung stage.
  WorkerEvent event;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline &&
         !executor->WaitForEvent(event, 1000)) {
  }
  executor->Cancel(request.task_id, "user aborted");

  const auto rest = DrainToTerminal(*executor, 2000);
  ASSERT_FALSE(rest.empty());
  EXPECT_EQ(rest.back().type, WorkerEventType::kCancelled);
}

}  // namespace
}  // namespace spatial::engine
