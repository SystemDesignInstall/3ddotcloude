#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/project/project.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/workers/child_process.h"

#ifndef SPATIAL_CLI_EXECUTABLE
#error SPATIAL_CLI_EXECUTABLE must be defined by the test build
#endif

namespace spatial::core {

using engine::ChildProcess;

namespace {

const std::string kCliExe = SPATIAL_CLI_EXECUTABLE;

// Minimal valid JPEG: SOI, APP0, SOF0 (8-bit, 4x2, 3 components), EOI.
const std::vector<std::uint8_t> kJpeg = {
    0xFF, 0xD8,
    0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x02, 0x00, 0x04, 0x03, 0x01,
    0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,
    0xFF, 0xD9,
};

// Second distinct valid JPEG (height 3 vs 2): different SOF0 -> different
// bytes -> distinct content hash, so two observations share one session.
const std::vector<std::uint8_t> kJpegAlt = {
    0xFF, 0xD8,
    0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01,
    0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xC0, 0x00, 0x11, 0x08, 0x00, 0x03, 0x00, 0x04, 0x03, 0x01,
    0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,
    0xFF, 0xD9,
};

struct CliRun {
  int exit_code = -1;
  std::string stdout_text;
};

CliRun RunCli(const std::vector<std::string>& argv) {
  std::string error;
  auto child = ChildProcess::Spawn(argv, error);
  if (!child) {
    ADD_FAILURE() << "cli spawn failed: " << error;
    return {-1, ""};
  }
  CliRun run;
  std::vector<char> buf(16384);
  while (true) {
    std::size_t n = 0;
    bool eof = false;
    std::string err;
    if (child->ReadStdout(buf.data(), buf.size(), 20000, n, eof, err)) {
      run.stdout_text.append(buf.data(), n);
    } else {
      run.stdout_text.append(buf.data(), n);
      if (eof) break;
      ADD_FAILURE() << "cli stdout read failed: " << err;
      child->Terminate();
      break;
    }
  }
  run.exit_code = child->Wait();
  return run;
}

void InsertSensor(Project& project, const Uuid& sensor_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO sensors (sensor_id, project_id, type, status)"
      " VALUES (?, ?, 'camera', 'active')";
  ASSERT_EQ(sqlite3_prepare_v2(project.db().db(), sql, -1, &stmt, nullptr),
            SQLITE_OK);
  sqlite3_bind_blob(stmt, 1, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, project.info().uuid.data(),
                    static_cast<int>(project.info().uuid.size()),
                    SQLITE_TRANSIENT);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
}

class CliFeatureExtractionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_cli_fe_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    project_path_ = root_ / "cli_fe.spx";
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "cli_fe";
    info.created_at = fs::Iso8601UtcNow();
    Project::Create(project_path_, info);
    sensor_id_ = GenerateUuid();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Registers a sensor then releases the project writer lock so the CLI child
  // process can open the project (Project::Open is exclusive, ADR-008).
  void SeedSensor() {
    {
      auto p = Project::Open(project_path_);
      InsertSensor(p, sensor_id_);
    }
  }

  std::filesystem::path WriteFile(const std::string& name,
                                  const std::vector<std::uint8_t>& bytes) {
    const auto path = root_ / name;
    fs::AtomicWrite(path, bytes);
    return path;
  }

  std::vector<std::string> ImportArgs(const std::vector<std::string>& files,
                                      const std::string& time_ns) {
    std::vector<std::string> args = {kCliExe, "import"};
    args.insert(args.end(), files.begin(), files.end());
    args.push_back("--sensor");
    args.push_back(FormatUuid(sensor_id_));
    args.push_back("--time");
    args.push_back(time_ns);
    args.push_back("--project");
    args.push_back(project_path_.string());
    return args;
  }

  // Imports two distinct images in one batch and returns the session_id.
  std::string ImportTwoImages() {
    const auto a = WriteFile("cam_a.jpg", kJpeg);
    const auto b = WriteFile("cam_b.jpg", kJpegAlt);
    const auto run =
        RunCli(ImportArgs({a.string(), b.string()}, "1723000000000000000"));
    EXPECT_EQ(run.exit_code, 0);
    const auto j = nlohmann::json::parse(run.stdout_text);
    EXPECT_EQ(j["imported"].size(), 2u);
    return j["session_id"].get<std::string>();
  }

  std::vector<std::string> FeatureArgs(const std::string& session_id,
                                       const std::string& config = "") {
    std::vector<std::string> args = {kCliExe, "run", "feature-extraction"};
    args.push_back("--session");
    args.push_back(session_id);
    if (!config.empty()) {
      args.push_back("--config");
      args.push_back(config);
    }
    args.push_back("--project");
    args.push_back(project_path_.string());
    return args;
  }

  std::filesystem::path root_;
  std::filesystem::path project_path_;
  Uuid sensor_id_{};
};

TEST_F(CliFeatureExtractionTest, FeatureExtractionThroughCliWritesFeatureSets) {
  SeedSensor();
  const std::string session_id = ImportTwoImages();

  const auto run = RunCli(FeatureArgs(session_id));
  ASSERT_EQ(run.exit_code, 0);

  const auto j = nlohmann::json::parse(run.stdout_text);
  EXPECT_EQ(j["session_id"].get<std::string>(), session_id);
  EXPECT_EQ(j["failures"].size(), 0u);
  ASSERT_EQ(j["feature_sets"].size(), 2u);
  for (const auto& fs : j["feature_sets"]) {
    EXPECT_EQ(fs["detector"].get<std::string>(), "mock");
    EXPECT_EQ(fs["descriptor_type"].get<std::string>(), "mock_16");
    EXPECT_EQ(fs["count"].get<std::int64_t>(), 64);
    EXPECT_FALSE(fs["feature_set_id"].get<std::string>().empty());
    EXPECT_FALSE(fs["artifact_ref"].get<std::string>().empty());
    EXPECT_EQ(fs["content_hash"].get<std::string>().size(), 64u);
  }
  const std::string first_id =
      j["feature_sets"][0]["feature_set_id"].get<std::string>();
  const std::string second_id =
      j["feature_sets"][1]["feature_set_id"].get<std::string>();
  EXPECT_NE(first_id, second_id);

  // Canonical records persisted: one feature_sets row per frame, one
  // FeatureArtifact (type "feature") per frame, image + feature payloads.
  auto p = Project::Open(project_path_);
  EXPECT_EQ(p.artifacts().PayloadCount(), 4u);
  EXPECT_EQ(p.db().FindArtifactsByType("feature").size(), 2u);
  for (const auto& fs : j["feature_sets"]) {
    const Uuid frame_id = ParseUuid(fs["frame_id"].get<std::string>());
    const auto rows = p.db().FindFeatureSetsByFrame(frame_id);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().detector, "mock");
    EXPECT_EQ(rows.front().descriptor_type, "mock_16");
    EXPECT_EQ(rows.front().count, 64);
  }

  // Provenance (P2.3 M1): every persisted FeatureArtifact manifest records the
  // effective stage configuration hash, and all instances of this run agree
  // (the worker's manifest is what persists; the CLI re-Put dedups to it).
  std::string expected_config_hash;
  for (const auto& row : p.db().FindArtifactsByType("feature")) {
    const auto manifest = p.artifacts().ReadManifest(row.artifact_id);
    ASSERT_TRUE(manifest.has_value());
    EXPECT_EQ(manifest->configuration_hash.size(), 64u);
    if (expected_config_hash.empty()) {
      expected_config_hash = manifest->configuration_hash;
    } else {
      EXPECT_EQ(manifest->configuration_hash, expected_config_hash);
    }
  }
  ASSERT_FALSE(expected_config_hash.empty());
}

TEST_F(CliFeatureExtractionTest, FeatureExtractionThroughCliIsIdempotent) {
  SeedSensor();
  const std::string session_id = ImportTwoImages();
  const auto args = FeatureArgs(session_id);

  const auto first = RunCli(args);
  ASSERT_EQ(first.exit_code, 0);
  const auto first_json = nlohmann::json::parse(first.stdout_text);
  ASSERT_EQ(first_json["feature_sets"].size(), 2u);

  // Run #2: same deterministic FeatureArtifacts, no duplicate feature_sets
  // records (DeriveFeatureSetId), no new CAS payloads.
  const auto second = RunCli(args);
  ASSERT_EQ(second.exit_code, 0);
  const auto second_json = nlohmann::json::parse(second.stdout_text);
  ASSERT_EQ(second_json["feature_sets"].size(), 2u);
  EXPECT_EQ(second_json["failures"].size(), 0u);
  for (std::size_t i = 0; i < 2u; ++i) {
    EXPECT_TRUE(second_json["feature_sets"][i]["deduplicated"].get<bool>());
    EXPECT_EQ(second_json["feature_sets"][i]["feature_set_id"].get<std::string>(),
              first_json["feature_sets"][i]["feature_set_id"].get<std::string>());
    EXPECT_EQ(second_json["feature_sets"][i]["content_hash"].get<std::string>(),
              first_json["feature_sets"][i]["content_hash"].get<std::string>());
  }

  // No duplicate logical record and unchanged CAS semantics.
  auto p = Project::Open(project_path_);
  EXPECT_EQ(p.artifacts().PayloadCount(), 4u);
  EXPECT_EQ(p.db().FindArtifactsByType("feature").size(), 2u);
  for (const auto& fs : second_json["feature_sets"]) {
    const Uuid frame_id = ParseUuid(fs["frame_id"].get<std::string>());
    EXPECT_EQ(p.db().FindFeatureSetsByFrame(frame_id).size(), 1u);
  }
}

TEST_F(CliFeatureExtractionTest, UnknownSessionIsRejected) {
  SeedSensor();
  const auto run = RunCli(FeatureArgs(FormatUuid(GenerateUuid())));
  EXPECT_EQ(run.exit_code, 1);
}

TEST_F(CliFeatureExtractionTest, ConfigurationHashReflectsEffectiveConfig) {
  SeedSensor();
  const std::string session_id = ImportTwoImages();

  const auto default_run = RunCli(FeatureArgs(session_id));
  ASSERT_EQ(default_run.exit_code, 0);
  const auto default_json = nlohmann::json::parse(default_run.stdout_text);
  ASSERT_EQ(default_json["feature_sets"].size(), 2u);

  const auto sized_run = RunCli(
      FeatureArgs(session_id, R"({"keypoint_count": 128})"));
  ASSERT_EQ(sized_run.exit_code, 0);
  const auto sized_json = nlohmann::json::parse(sized_run.stdout_text);
  ASSERT_EQ(sized_json["feature_sets"].size(), 2u);
  EXPECT_EQ(sized_json["failures"].size(), 0u);

  const std::string default_ref =
      default_json["feature_sets"][0]["content_hash"].get<std::string>();
  const std::string sized_ref =
      sized_json["feature_sets"][0]["content_hash"].get<std::string>();
  EXPECT_EQ(default_ref.size(), 64u);
  EXPECT_EQ(sized_ref.size(), 64u);
  // A different config changes the derived payload, hence the content hash.
  EXPECT_NE(sized_ref, default_ref);

  // The persisted FeatureArtifact manifest's configuration_hash must reflect
  // the effective stage configuration (P2.3 M1): differ between the two runs.
  std::string default_hash;
  std::string sized_hash;
  {
    auto p = Project::Open(project_path_);
    const auto default_indexed = p.db().FindArtifactByHash(default_ref);
    ASSERT_TRUE(default_indexed.has_value());
    const auto default_manifest =
        p.artifacts().ReadManifest(default_indexed->artifact_id);
    ASSERT_TRUE(default_manifest.has_value());
    default_hash = default_manifest->configuration_hash;
    EXPECT_EQ(default_hash.size(), 64u);

    const auto sized_indexed = p.db().FindArtifactByHash(sized_ref);
    ASSERT_TRUE(sized_indexed.has_value());
    const auto sized_manifest =
        p.artifacts().ReadManifest(sized_indexed->artifact_id);
    ASSERT_TRUE(sized_manifest.has_value());
    sized_hash = sized_manifest->configuration_hash;
    EXPECT_EQ(sized_hash.size(), 64u);
  }
  EXPECT_NE(sized_hash, default_hash);
}

}  // namespace

}  // namespace spatial::core
