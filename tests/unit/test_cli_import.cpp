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

const std::vector<std::uint8_t> kGarbage = {0x01, 0x02, 0x03, 0x04, 0x05};

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

class CliImportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_cli_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    project_path_ = root_ / "cli.spx";
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = "cli";
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

  std::filesystem::path root_;
  std::filesystem::path project_path_;
  Uuid sensor_id_{};
};

TEST_F(CliImportTest, ImportThroughCliWritesCanonicalRecords) {
  SeedSensor();
  const auto img = WriteFile("cam.jpg", kJpeg);
  const auto run = RunCli(ImportArgs({img.string()}, "1723000000000000000"));
  ASSERT_EQ(run.exit_code, 0);

  const auto j = nlohmann::json::parse(run.stdout_text);
  EXPECT_FALSE(j["session_id"].get<std::string>().empty());
  EXPECT_EQ(j["failures"].size(), 0u);
  ASSERT_EQ(j["imported"].size(), 1u);
  const auto& e = j["imported"][0];
  EXPECT_EQ(e["width"].get<std::int64_t>(), 4);
  EXPECT_EQ(e["height"].get<std::int64_t>(), 2);
  EXPECT_EQ(e["pixel_format"].get<std::string>(), "rgb8");
  EXPECT_EQ(e["mime_type"].get<std::string>(), "image/jpeg");
  EXPECT_FALSE(e["reimported"].get<bool>());
  EXPECT_FALSE(e["new_instance"].get<bool>());

  // Canonical records persisted: reopen the project and verify the chain
  // artifact -> frame -> observation.
  auto p = Project::Open(project_path_);
  EXPECT_EQ(p.artifacts().PayloadCount(), 1u);
  EXPECT_EQ(p.db().FindArtifactsByType("image").size(), 1u);
  const Uuid frame_id = ParseUuid(e["frame_id"].get<std::string>());
  const Uuid obs_id = ParseUuid(e["observation_id"].get<std::string>());
  EXPECT_TRUE(p.db().FrameExists(frame_id));
  EXPECT_TRUE(p.db().ObservationExists(obs_id));
}

TEST_F(CliImportTest, ReimportThroughCliIsIdempotent) {
  SeedSensor();
  const auto img = WriteFile("cam.jpg", kJpeg);
  const auto args = ImportArgs({img.string()}, "1723000000000000000");

  const auto first = RunCli(args);
  ASSERT_EQ(first.exit_code, 0);
  const auto second = RunCli(args);
  ASSERT_EQ(second.exit_code, 0);

  const auto j = nlohmann::json::parse(second.stdout_text);
  ASSERT_EQ(j["imported"].size(), 1u);
  EXPECT_TRUE(j["imported"][0]["reimported"].get<bool>());

  // Dedup case 1: re-import creates no new artifact instance or observation.
  auto p = Project::Open(project_path_);
  EXPECT_EQ(p.artifacts().PayloadCount(), 1u);
  EXPECT_EQ(p.db().FindArtifactsByType("image").size(), 1u);
}

TEST_F(CliImportTest, FailuresArePerFileAndBatchContinues) {
  SeedSensor();
  const auto good = WriteFile("good.jpg", kJpeg);
  const auto bad = WriteFile("bad.bin", kGarbage);
  const auto run =
      RunCli(ImportArgs({bad.string(), good.string()}, "1"));
  EXPECT_EQ(run.exit_code, 1);

  const auto j = nlohmann::json::parse(run.stdout_text);
  ASSERT_EQ(j["imported"].size(), 1u);
  ASSERT_EQ(j["failures"].size(), 1u);
  EXPECT_EQ(j["failures"][0]["code"].get<std::string>(),
            "IMPORT_UNSUPPORTED_FORMAT");
  EXPECT_EQ(j["imported"][0]["width"].get<std::int64_t>(), 4);
}

TEST_F(CliImportTest, UnknownFlagIsRejected) {
  SeedSensor();
  const auto img = WriteFile("cam.jpg", kJpeg);
  auto args = ImportArgs({img.string()}, "1");
  args.push_back("--bogus");
  const auto run = RunCli(args);
  EXPECT_EQ(run.exit_code, 1);
}

}  // namespace

}  // namespace spatial::core
