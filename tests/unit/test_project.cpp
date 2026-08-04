#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "core/errors/project_error.h"
#include "core/project/project.h"
#include "core/utils/sha256.h"

namespace spatial::core {
namespace {

class ProjectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_prj_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  static ProjectInfo MakeInfo(const std::string& name) {
    ProjectInfo info;
    info.uuid = GenerateUuid();
    info.name = name;
    info.created_by.version = "0.1.0";
    info.created_by.git_commit = "abc123";
    return info;
  }

  std::filesystem::path root_;
};

TEST_F(ProjectTest, CreateProducesSpxLayout) {
  const auto p = Project::Create(root_ / "demo.spx", MakeInfo("demo"));
  ASSERT_TRUE(p.IsOpen());
  EXPECT_TRUE(std::filesystem::exists(root_ / "demo.spx" / "project.json"));
  EXPECT_TRUE(std::filesystem::exists(root_ / "demo.spx" / "project.db"));
  for (const auto& dir :
       {"artifacts", "cache", "logs", "temp"}) {
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "demo.spx" / dir));
  }
  EXPECT_EQ(p.info().name, "demo");
  EXPECT_FALSE(p.read_only());
}

TEST_F(ProjectTest, RejectsNonSpxName) {
  EXPECT_THROW(Project::Create(root_ / "demo", MakeInfo("demo")), StorageError);
}

TEST_F(ProjectTest, RejectsExistingProject) {
  const auto path = root_ / "demo.spx";
  Project::Create(path, MakeInfo("demo"));
  EXPECT_THROW(Project::Create(path, MakeInfo("demo2")), StorageError);
}

TEST_F(ProjectTest, OpenRoundTrip) {
  const auto path = root_ / "demo.spx";
  {
    auto p = Project::Create(path, MakeInfo("demo"));
    p.Save();
  }
  auto p = Project::Open(path);
  ASSERT_TRUE(p.IsOpen());
  EXPECT_EQ(p.info().name, "demo");
  EXPECT_EQ(p.root(), path);
  EXPECT_FALSE(p.read_only());
}

TEST_F(ProjectTest, OpenReadOnly) {
  const auto path = root_ / "demo.spx";
  Project::Create(path, MakeInfo("demo"));
  auto p = Project::Open(path, /*read_only=*/true);
  EXPECT_TRUE(p.read_only());
}

TEST_F(ProjectTest, OpenMissingThrows) {
  EXPECT_THROW(Project::Open(root_ / "nope.spx"), StorageError);
}

TEST_F(ProjectTest, ArtifactLifecycleThroughProject) {
  const auto path = root_ / "demo.spx";
  {
    auto p = Project::Create(path, MakeInfo("demo"));
    const auto payload = std::vector<std::uint8_t>{9, 8, 7, 6};
    auto result = p.artifacts().Put(payload, [&] {
      ArtifactManifest m;
      m.type = "pointcloud";
      m.producer.id = "proj-test";
      m.producer.version = "1.0.0";
      m.mime_type = "application/x-ply";
      return m;
    }());
    EXPECT_TRUE(p.artifacts().Has(result.content_hash));
    EXPECT_EQ(p.artifacts().Get(result.content_hash).value(), payload);
    p.VerifyIntegrity();
  }
  // Reopen: artifacts persisted across close/open.
  auto p2 = Project::Open(path);
  EXPECT_EQ(p2.artifacts().PayloadCount(), 1u);
  p2.VerifyIntegrity();
}

TEST_F(ProjectTest, CorruptionDetected) {
  const auto path = root_ / "demo.spx";
  {
    auto p = Project::Create(path, MakeInfo("demo"));
    const auto payload = std::vector<std::uint8_t>{1, 1, 2, 3, 5};
    auto result = p.artifacts().Put(payload, [&] {
      ArtifactManifest m;
      m.type = "mesh";
      m.producer.id = "proj-test";
      m.producer.version = "1.0.0";
      return m;
    }());
    const auto payload_path = p.artifacts_root() / "cas" /
                              result.content_hash.substr(0, 2) /
                              result.content_hash;
    {
      std::ofstream out(payload_path, std::ios::binary | std::ios::trunc);
      out.put(0x00);
    }
  }
  auto p2 = Project::Open(path);
  EXPECT_THROW(p2.VerifyIntegrity(), ArtifactError);
}

TEST_F(ProjectTest, SaveRejectsReadOnly) {
  const auto path = root_ / "demo.spx";
  Project::Create(path, MakeInfo("demo"));
  auto p = Project::Open(path, /*read_only=*/true);
  EXPECT_THROW(p.Save(), StorageError);
}

}  // namespace
}  // namespace spatial::core
