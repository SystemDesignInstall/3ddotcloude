#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/errors/project_error.h"

namespace spatial::core {
namespace {

TEST(Errors, HierarchyAndCodes) {
  StorageError e(ErrorCode::kStorageReadOnly, "read only");
  EXPECT_EQ(e.code(), ErrorCode::kStorageReadOnly);
  EXPECT_EQ(e.domain(), ErrorDomain::kStorage);
  EXPECT_EQ(std::string(e.what()), "STORAGE: read only");

  ArtifactError a(ErrorCode::kArtifactHashMismatch, "hash mismatch");
  EXPECT_EQ(a.domain(), ErrorDomain::kArtifact);
  EXPECT_STREQ(ErrorCodeName(ErrorCode::kArtifactHashMismatch),
               "ARTIFACT_HASH_MISMATCH");
}

TEST(Errors, ContextAndRecoverable) {
  StorageError e(ErrorCode::kStorageIo, "io", {{"path", "/tmp/x"}},
                 /*recoverable=*/true, "Free disk space.");
  EXPECT_TRUE(e.recoverable());
  ASSERT_EQ(e.context().size(), 1u);
  EXPECT_EQ(e.context()[0].key, "path");
  EXPECT_EQ(e.context()[0].value, "/tmp/x");
  EXPECT_EQ(e.suggested_action(), "Free disk space.");
}

TEST(Errors, CauseChaining) {
  auto cause = std::make_shared<SchemaError>(ErrorCode::kSchemaInvalid,
                                             "bad column");
  ArtifactError e(ErrorCode::kArtifactManifest, "manifest bad",
                  std::vector<ErrorContext>{}, false, "",
                  std::static_pointer_cast<const ProjectError>(cause));
  ASSERT_TRUE(e.cause());
  EXPECT_EQ(e.cause()->code(), ErrorCode::kSchemaInvalid);
}

TEST(Errors, StableCodeString) {
  EXPECT_EQ(StableErrorCode(ErrorCode::kStorageAtomicWrite),
            "STORAGE_ATOMIC_WRITE");
  EXPECT_EQ(StableErrorCode(ErrorCode::kProjectNotFound), "PROJECT_NOT_FOUND");
  EXPECT_EQ(StableErrorCode(ErrorCode::kValidationSchema), "VALIDATION_SCHEMA");
}

}  // namespace
}  // namespace spatial::core
