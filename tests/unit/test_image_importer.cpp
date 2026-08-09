#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "core/artifacts/artifact_store.h"
#include "core/errors/project_error.h"
#include "core/scene/identity.h"
#include "core/storage/metadata_db.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"
#include "importers/images/image_importer.h"

namespace spatial::core {

using importers::ImageImportEntry;
using importers::ImageImportFailure;
using importers::ImageImportResult;
using importers::ImageImporter;
using importers::ImageImporterConfig;
using importers::ImageSourceFile;

namespace {

const Uuid kProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

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

void InsertTestProject(MetadataDb& db) {
  db.InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978", "local",
                   "{}", "{}");
}

void InsertTestSensor(MetadataDb& db, const Uuid& sensor_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO sensors (sensor_id, project_id, type)"
      " VALUES (?, ?, 'camera')";
  ASSERT_EQ(sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
  sqlite3_bind_blob(stmt, 1, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, kProjectId.data(),
                    static_cast<int>(kProjectId.size()), SQLITE_TRANSIENT);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
}

void InsertTestCalibration(MetadataDb& db, const Uuid& sensor_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO calibrations (calibration_id, sensor_id, version)"
      " VALUES (?, ?, 1)";
  ASSERT_EQ(sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr), SQLITE_OK);
  const Uuid cal_id = GenerateUuid();
  sqlite3_bind_blob(stmt, 1, cal_id.data(), static_cast<int>(cal_id.size()),
                    SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, sensor_id.data(),
                    static_cast<int>(sensor_id.size()), SQLITE_TRANSIENT);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
  sqlite3_finalize(stmt);
}

std::optional<std::string> ArtifactRefOf(MetadataDb& db,
                                         const Uuid& observation_id) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT artifact_ref FROM observations WHERE observation_id = ?";
  if (sqlite3_prepare_v2(db.db(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_blob(stmt, 1, observation_id.data(),
                    static_cast<int>(observation_id.size()), SQLITE_TRANSIENT);
  std::optional<std::string> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* t = sqlite3_column_text(stmt, 0);
    if (t) out = reinterpret_cast<const char*>(t);
  }
  sqlite3_finalize(stmt);
  return out;
}

class ImageImporterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_imgimp_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = MetadataDb::Create(root_ / "project.db");
    InsertTestProject(*db_);
    store_ = std::make_unique<ArtifactStore>(root_ / "artifacts", *db_);
    config_.batch_name = "import";
  }

  void TearDown() override {
    store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path WriteImage(const std::string& name,
                                   const std::vector<std::uint8_t>& bytes) {
    const auto path = root_ / name;
    fs::AtomicWrite(path, bytes);
    return path;
  }

  ImageSourceFile Source(const std::filesystem::path& path,
                         const std::string& uri, const Uuid& sensor_id,
                         std::int64_t ts) {
    ImageSourceFile f;
    f.path = path;
    f.source_uri = uri;
    f.sensor_id = sensor_id;
    f.timestamp_ns = ts;
    return f;
  }

  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> store_;
  ImageImporterConfig config_;
};

TEST_F(ImageImporterTest, ImportCreatesArtifactFrameObservation) {
  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(*db_, sensor_id);
  InsertTestCalibration(*db_, sensor_id);
  const auto path = WriteImage("cam.jpg", kJpeg);

  ImageImporter importer(*store_, *db_, kProjectId, config_);
  const auto result = importer.Import(
      {Source(path, "file:///cam.jpg", sensor_id, 1'700'000'000'000ULL)});

  EXPECT_EQ(result.failures.size(), 0u);
  ASSERT_EQ(result.imported.size(), 1u);
  const auto& e = result.imported[0];
  EXPECT_FALSE(IsNil(result.session_id));
  EXPECT_FALSE(e.reimported);
  EXPECT_FALSE(e.new_instance);
  EXPECT_FALSE(e.uncalibrated);
  EXPECT_EQ(e.content_hash, Sha256Hex(kJpeg));
  EXPECT_EQ(e.width, 4);
  EXPECT_EQ(e.height, 2);
  EXPECT_EQ(e.pixel_format, "rgb8");
  EXPECT_EQ(e.mime_type, "image/jpeg");
  EXPECT_EQ(e.frame_id,
            DeriveFrameId(sensor_id, 1'700'000'000'000ULL, e.content_hash));
  EXPECT_EQ(e.observation_id, DeriveObservationId(sensor_id,
                                                  1'700'000'000'000ULL,
                                                  e.content_hash));

  // One CAS payload, one manifest, one index row, canonical records.
  EXPECT_EQ(store_->PayloadCount(), 1u);
  EXPECT_TRUE(store_->HasManifest(e.artifact_uuid));
  const auto manifest = store_->ReadManifest(e.artifact_uuid);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->content_hash, e.content_hash);
  EXPECT_EQ(manifest->type, "image");
  EXPECT_EQ(manifest->width, 4);
  EXPECT_EQ(manifest->height, 2);
  EXPECT_EQ(manifest->pixel_format, "rgb8");
  EXPECT_EQ(manifest->mime_type, "image/jpeg");
  EXPECT_EQ(manifest->coordinate_frame, "image");
  EXPECT_FALSE(manifest->configuration_hash.empty());
  EXPECT_EQ(manifest->producer.id, "image-import");

  const auto rows = db_->FindArtifactsByType("image");
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].artifact_id, e.artifact_uuid);

  EXPECT_TRUE(db_->FrameExists(e.frame_id));
  EXPECT_TRUE(db_->ObservationExists(e.observation_id));
  const auto ref = ArtifactRefOf(*db_, e.observation_id);
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(*ref, FormatUuid(e.artifact_uuid));
}

TEST_F(ImageImporterTest, ReimportIsIdempotent) {
  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(*db_, sensor_id);
  const auto path = WriteImage("cam.jpg", kJpeg);
  ImageImporter importer(*store_, *db_, kProjectId, config_);
  const auto input =
      Source(path, "file:///cam.jpg", sensor_id, 1'700'000'000'000ULL);

  const auto first = importer.Import({input});
  ASSERT_EQ(first.imported.size(), 1u);

  // Dedup case 1: identical tuple re-imports to the same IDs, no new records.
  const auto second = importer.Import({input});
  ASSERT_EQ(second.imported.size(), 1u);
  EXPECT_EQ(second.failures.size(), 0u);
  EXPECT_TRUE(second.imported[0].reimported);
  EXPECT_FALSE(second.imported[0].new_instance);
  EXPECT_TRUE(IsNil(second.imported[0].artifact_uuid));
  EXPECT_EQ(second.imported[0].frame_id, first.imported[0].frame_id);
  EXPECT_EQ(second.imported[0].observation_id,
            first.imported[0].observation_id);
  EXPECT_EQ(store_->PayloadCount(), 1u);
  EXPECT_EQ(db_->FindArtifactsByType("image").size(), 1u);
  EXPECT_EQ(db_->FrameExists(first.imported[0].frame_id), true);
}

TEST_F(ImageImporterTest, DedupCase2SharedPayloadTwoInstances) {
  // Identical bytes, different camera -> one CAS payload, two instances, two
  // observations, two distinct v5 IDs (image-import.md §13 case 2).
  const Uuid sensor_a = GenerateUuid();
  const Uuid sensor_b = GenerateUuid();
  InsertTestSensor(*db_, sensor_a);
  InsertTestSensor(*db_, sensor_b);
  const auto path = WriteImage("cam.jpg", kJpeg);
  ImageImporter importer(*store_, *db_, kProjectId, config_);

  const auto first = importer.Import(
      {Source(path, "file:///a.jpg", sensor_a, 1'700'000'000'000ULL)});
  ASSERT_EQ(first.imported.size(), 1u);
  EXPECT_FALSE(first.imported[0].new_instance);

  const auto second = importer.Import(
      {Source(path, "file:///b.jpg", sensor_b, 1'700'000'000'000ULL)});
  ASSERT_EQ(second.imported.size(), 1u);
  EXPECT_TRUE(second.imported[0].new_instance);
  EXPECT_EQ(second.imported[0].content_hash, first.imported[0].content_hash);
  EXPECT_NE(second.imported[0].artifact_uuid, first.imported[0].artifact_uuid);
  EXPECT_NE(second.imported[0].observation_id,
            first.imported[0].observation_id);
  EXPECT_NE(second.imported[0].frame_id, first.imported[0].frame_id);

  EXPECT_EQ(store_->PayloadCount(), 1u);
  EXPECT_TRUE(store_->HasManifest(first.imported[0].artifact_uuid));
  EXPECT_TRUE(store_->HasManifest(second.imported[0].artifact_uuid));
  EXPECT_EQ(db_->FindArtifactsByType("image").size(), 1u);
  EXPECT_TRUE(db_->ObservationExists(first.imported[0].observation_id));
  EXPECT_TRUE(db_->ObservationExists(second.imported[0].observation_id));
  const auto ref = ArtifactRefOf(*db_, second.imported[0].observation_id);
  ASSERT_TRUE(ref.has_value());
  EXPECT_EQ(*ref, FormatUuid(second.imported[0].artifact_uuid));
}

TEST_F(ImageImporterTest, DedupCase2SameSensorDifferentTime) {
  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(*db_, sensor_id);
  const auto path = WriteImage("cam.jpg", kJpeg);
  ImageImporter importer(*store_, *db_, kProjectId, config_);

  const auto first = importer.Import(
      {Source(path, "file:///t1.jpg", sensor_id, 1'700'000'000'000ULL)});
  const auto second = importer.Import(
      {Source(path, "file:///t2.jpg", sensor_id, 1'700'000'000'010ULL)});
  ASSERT_EQ(first.imported.size(), 1u);
  ASSERT_EQ(second.imported.size(), 1u);
  EXPECT_TRUE(second.imported[0].new_instance);
  EXPECT_NE(second.imported[0].observation_id,
            first.imported[0].observation_id);
  EXPECT_EQ(store_->PayloadCount(), 1u);
}

TEST_F(ImageImporterTest, PerFileFailuresDoNotAbortBatch) {
  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(*db_, sensor_id);
  const auto good = WriteImage("good.jpg", kJpeg);
  const auto bad = WriteImage("bad.bin", kGarbage);
  const auto missing = root_ / "missing.jpg";
  ImageImporter importer(*store_, *db_, kProjectId, config_);

  const auto result = importer.Import(
      {Source(bad, "file:///bad.bin", sensor_id, 1), Source(good, "file:///good.jpg", sensor_id, 2),
       Source(missing, "file:///missing.jpg", sensor_id, 3)});

  ASSERT_EQ(result.imported.size(), 1u);
  EXPECT_EQ(result.failures.size(), 2u);
  EXPECT_EQ(result.imported[0].content_hash, Sha256Hex(kJpeg));
  EXPECT_EQ(result.failures[0].code, ErrorCode::kImportUnsupportedFormat);
  EXPECT_EQ(result.failures[0].source_uri, "file:///bad.bin");
  EXPECT_EQ(result.failures[1].code, ErrorCode::kImportUnreadable);
  EXPECT_EQ(result.failures[1].source_uri, "file:///missing.jpg");
  EXPECT_EQ(store_->PayloadCount(), 1u);
  EXPECT_EQ(db_->FindArtifactsByType("image").size(), 1u);
}

TEST_F(ImageImporterTest, SensorUnresolvedFails) {
  const auto path = WriteImage("cam.jpg", kJpeg);
  ImageImporter importer(*store_, *db_, kProjectId, config_);
  const auto result = importer.Import(
      {Source(path, "file:///cam.jpg", GenerateUuid(), 1)});
  EXPECT_EQ(result.imported.size(), 0u);
  ASSERT_EQ(result.failures.size(), 1u);
  EXPECT_EQ(result.failures[0].code, ErrorCode::kImportSensorUnresolved);
  EXPECT_EQ(store_->PayloadCount(), 0u);
}

TEST_F(ImageImporterTest, EmptyBatchThrows) {
  ImageImporter importer(*store_, *db_, kProjectId, config_);
  EXPECT_THROW(importer.Import({}), ImportError);
}

TEST_F(ImageImporterTest, ExplicitSessionUsedAndValidated) {
  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(*db_, sensor_id);
  const auto path = WriteImage("cam.jpg", kJpeg);

  CaptureSessionRow row;
  row.session_id = GenerateUuid();
  row.project_id = kProjectId;
  row.name = "explicit";
  db_->InsertCaptureSession(row);

  ImageImporter importer(*store_, *db_, kProjectId, config_);
  const auto result = importer.Import(
      {Source(path, "file:///cam.jpg", sensor_id, 1)}, row.session_id);
  EXPECT_EQ(result.session_id, row.session_id);
  ASSERT_EQ(result.imported.size(), 1u);

  EXPECT_THROW(importer.Import({Source(path, "file:///cam.jpg", sensor_id, 1)},
                               GenerateUuid()),
               ImportError);
}

TEST_F(ImageImporterTest, UncalibratedSensorIsWarningNotFailure) {
  const Uuid sensor_id = GenerateUuid();
  InsertTestSensor(*db_, sensor_id);  // no calibration
  const auto path = WriteImage("cam.jpg", kJpeg);
  ImageImporter importer(*store_, *db_, kProjectId, config_);

  const auto result = importer.Import(
      {Source(path, "file:///cam.jpg", sensor_id, 1)});
  ASSERT_EQ(result.imported.size(), 1u);
  EXPECT_TRUE(result.imported[0].uncalibrated);
  EXPECT_EQ(result.failures.size(), 0u);

  // Registering a calibration flips the warning on the next import context.
  InsertTestCalibration(*db_, sensor_id);
  const auto second = importer.Import(
      {Source(path, "file:///cam2.jpg", sensor_id, 2)});
  ASSERT_EQ(second.imported.size(), 1u);
  EXPECT_FALSE(second.imported[0].uncalibrated);
}

}  // namespace
}  // namespace spatial::core
