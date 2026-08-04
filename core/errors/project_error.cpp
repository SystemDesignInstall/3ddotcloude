#include "core/errors/project_error.h"

#include <sstream>
#include <utility>

namespace spatial::core {

ErrorDomain DomainOf(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kProjectInvalid:
    case ErrorCode::kProjectNotFound:
    case ErrorCode::kProjectCorrupt:
      return ErrorDomain::kProject;
    case ErrorCode::kStorageCorrupt:
    case ErrorCode::kStorageReadOnly:
    case ErrorCode::kStorageLock:
    case ErrorCode::kStorageIo:
    case ErrorCode::kStorageAtomicWrite:
      return ErrorDomain::kStorage;
    case ErrorCode::kSchemaVersion:
    case ErrorCode::kSchemaMigration:
    case ErrorCode::kSchemaInvalid:
      return ErrorDomain::kSchema;
    case ErrorCode::kArtifactHashMismatch:
    case ErrorCode::kArtifactMissing:
    case ErrorCode::kArtifactManifest:
    case ErrorCode::kArtifactGcConflict:
    case ErrorCode::kArtifactUnreferenced:
      return ErrorDomain::kArtifact;
    case ErrorCode::kValidationSchema:
    case ErrorCode::kValidationDomain:
      return ErrorDomain::kValidation;
    default:
      return ErrorDomain::kProject;
  }
}

const char* DomainName(ErrorDomain domain) noexcept {
  switch (domain) {
    case ErrorDomain::kProject:
      return "PROJECT";
    case ErrorDomain::kStorage:
      return "STORAGE";
    case ErrorDomain::kSchema:
      return "SCHEMA";
    case ErrorDomain::kCoordinate:
      return "COORDINATE";
    case ErrorDomain::kCalibration:
      return "CALIBRATION";
    case ErrorDomain::kImport:
      return "IMPORT";
    case ErrorDomain::kArtifact:
      return "ARTIFACT";
    case ErrorDomain::kScheduler:
      return "SCHEDULER";
    case ErrorDomain::kWorker:
      return "WORKER";
    case ErrorDomain::kAdapter:
      return "ADAPTER";
    case ErrorDomain::kValidation:
      return "VALIDATION";
    default:
      return "UNKNOWN";
  }
}

const char* ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kProjectInvalid:
      return "PROJECT_INVALID";
    case ErrorCode::kProjectNotFound:
      return "PROJECT_NOT_FOUND";
    case ErrorCode::kProjectCorrupt:
      return "PROJECT_CORRUPT";
    case ErrorCode::kStorageCorrupt:
      return "STORAGE_CORRUPT";
    case ErrorCode::kStorageReadOnly:
      return "STORAGE_READ_ONLY";
    case ErrorCode::kStorageLock:
      return "STORAGE_LOCK";
    case ErrorCode::kStorageIo:
      return "STORAGE_IO";
    case ErrorCode::kStorageAtomicWrite:
      return "STORAGE_ATOMIC_WRITE";
    case ErrorCode::kSchemaVersion:
      return "SCHEMA_VERSION";
    case ErrorCode::kSchemaMigration:
      return "SCHEMA_MIGRATION";
    case ErrorCode::kSchemaInvalid:
      return "SCHEMA_INVALID";
    case ErrorCode::kArtifactHashMismatch:
      return "ARTIFACT_HASH_MISMATCH";
    case ErrorCode::kArtifactMissing:
      return "ARTIFACT_MISSING";
    case ErrorCode::kArtifactManifest:
      return "ARTIFACT_MANIFEST";
    case ErrorCode::kArtifactGcConflict:
      return "ARTIFACT_GC_CONFLICT";
    case ErrorCode::kArtifactUnreferenced:
      return "ARTIFACT_UNREFERENCED";
    case ErrorCode::kValidationSchema:
      return "VALIDATION_SCHEMA";
    case ErrorCode::kValidationDomain:
      return "VALIDATION_DOMAIN";
    default:
      return "INTERNAL";
  }
}

std::string StableErrorCode(ErrorCode code) {
  return ErrorCodeName(code);
}

ProjectError::ProjectError(ErrorCode code, std::string message,
                           std::vector<ErrorContext> context, bool recoverable,
                           std::string suggested_action,
                           std::shared_ptr<const ProjectError> cause)
    : code_(code),
      message_(std::move(message)),
      context_(std::move(context)),
      recoverable_(recoverable),
      suggested_action_(std::move(suggested_action)),
      cause_(std::move(cause)) {
  std::ostringstream oss;
  oss << DomainName(DomainOf(code_)) << ": " << message_;
  what_ = oss.str();
}

ProjectError::~ProjectError() = default;

const char* ProjectError::what() const noexcept { return what_.c_str(); }

}  // namespace spatial::core
