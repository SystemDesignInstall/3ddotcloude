#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "core/errors/project_error.h"
#include "core/storage/metadata_db.h"

namespace spatial::core {
namespace {

class MetadataDbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_mdb_" + std::to_string(std::time(nullptr)) + "_" +
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

TEST_F(MetadataDbTest, CreateAppliesMigrations) {
  auto db = MetadataDb::Create(path_);
  ASSERT_TRUE(db.IsOpen());
  EXPECT_FALSE(db.read_only());
  EXPECT_TRUE(std::filesystem::exists(path_));
  // Opening again must not re-apply.
  auto db2 = MetadataDb::Open(path_);
  EXPECT_EQ(db2.ApplyMigrations(), 0u);
}

TEST_F(MetadataDbTest, MigrationIdempotent) {
  auto db = MetadataDb::Create(path_);
  EXPECT_EQ(db.ApplyMigrations(), 0u);
  auto db2 = MetadataDb::Open(path_);
  EXPECT_EQ(db2.ApplyMigrations(), 0u);
}

TEST_F(MetadataDbTest, InsertAndFindArtifact) {
  auto db = MetadataDb::Create(path_);
  const auto uuid = GenerateUuid();
  ArtifactIndexRow row;
  row.artifact_id = uuid;
  row.content_hash = "abc123";
  row.type = "pointcloud";
  row.producer_json = "{}";
  row.created_at_ns = 12345;
  row.coordinate_frame = "scene";
  row.unit = "meter";
  row.file_size = 10;
  row.mime_type = "application/x-ply";
  db.UpsertArtifact(row);

  const auto found = db.FindArtifactByHash("abc123");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->artifact_id, uuid);
  EXPECT_EQ(found->type, "pointcloud");
  EXPECT_EQ(found->file_size, 10);
  EXPECT_EQ(found->validation_status, "valid");

  EXPECT_FALSE(db.FindArtifactByHash("zzz").has_value());
  const auto by_type = db.FindArtifactsByType("pointcloud");
  ASSERT_EQ(by_type.size(), 1u);
  EXPECT_EQ(by_type[0].content_hash, "abc123");
}

TEST_F(MetadataDbTest, DedupUpsertKeepsOneRow) {
  auto db = MetadataDb::Create(path_);
  const auto uuid = GenerateUuid();
  ArtifactIndexRow row;
  row.artifact_id = uuid;
  row.content_hash = "hash1";
  row.type = "mesh";
  row.producer_json = "{}";
  db.UpsertArtifact(row);

  ArtifactIndexRow row2 = row;
  row2.file_size = 99;
  db.UpsertArtifact(row2);

  EXPECT_EQ(db.FindArtifactsByType("mesh").size(), 1u);
  EXPECT_EQ(db.FindArtifactByHash("hash1")->file_size, 99);
}

TEST_F(MetadataDbTest, RecordDependency) {
  auto db = MetadataDb::Create(path_);
  EXPECT_NO_THROW(db.RecordDependency("in1", "out1", "input"));
  EXPECT_NO_THROW(db.RecordDependency("in2", "out1", "input"));
}

TEST_F(MetadataDbTest, ReadOnlyRejectsWrites) {
  auto db = MetadataDb::Create(path_);
  db.Close();
  auto ro = MetadataDb::OpenReadOnly(path_);
  EXPECT_TRUE(ro.read_only());
  ArtifactIndexRow row;
  row.artifact_id = GenerateUuid();
  row.content_hash = "h";
  row.type = "t";
  row.producer_json = "{}";
  EXPECT_THROW(ro.UpsertArtifact(row), StorageError);
}

TEST_F(MetadataDbTest, OpenMissingThrows) {
  EXPECT_THROW(MetadataDb::Open(root_ / "nope.db"), StorageError);
}

}  // namespace
}  // namespace spatial::core
