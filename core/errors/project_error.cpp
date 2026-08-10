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
    case ErrorCode::kCalibrationInvalid:
      return ErrorDomain::kCalibration;
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
    case ErrorCode::kCoordFrameMismatch:
    case ErrorCode::kCoordFrameNotFound:
    case ErrorCode::kCoordFrameCycle:
    case ErrorCode::kCoordFrameDisconnected:
    case ErrorCode::kCoordFrameMultipleRoots:
    case ErrorCode::kCoordFrameExists:
      return ErrorDomain::kCoordinate;
    case ErrorCode::kSchedDagCycle:
    case ErrorCode::kSchedDagTypeMismatch:
    case ErrorCode::kSchedDagResourceInfeasible:
    case ErrorCode::kSchedTaskUnknown:
    case ErrorCode::kSchedPersistence:
    case ErrorCode::kSchedCacheMiss:
    case ErrorCode::kSchedCancelled:
      return ErrorDomain::kScheduler;
    case ErrorCode::kWorkerProtocol:
    case ErrorCode::kWorkerHeartbeatTimeout:
    case ErrorCode::kWorkerCrashed:
    case ErrorCode::kWorkerTerminated:
    case ErrorCode::kWorkerBusy:
      return ErrorDomain::kWorker;
    case ErrorCode::kValidationSchema:
    case ErrorCode::kValidationDomain:
      return ErrorDomain::kValidation;
    case ErrorCode::kImportUnreadable:
    case ErrorCode::kImportCorrupt:
    case ErrorCode::kImportUnsupportedFormat:
    case ErrorCode::kImportMissingExif:
    case ErrorCode::kImportSensorUnresolved:
    case ErrorCode::kImportTimestampUnresolvable:
    case ErrorCode::kImportValidationError:
      return ErrorDomain::kImport;
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
    case ErrorCode::kCoordFrameMismatch:
      return "COORD_FRAME_MISMATCH";
    case ErrorCode::kCoordFrameNotFound:
      return "COORD_FRAME_NOT_FOUND";
    case ErrorCode::kCoordFrameCycle:
      return "COORD_FRAME_CYCLE";
    case ErrorCode::kCoordFrameDisconnected:
      return "COORD_FRAME_DISCONNECTED";
    case ErrorCode::kCoordFrameMultipleRoots:
      return "COORD_FRAME_MULTIPLE_ROOTS";
    case ErrorCode::kCoordFrameExists:
      return "COORD_FRAME_EXISTS";
    case ErrorCode::kSchedDagCycle:
      return "SCHED_DAG_CYCLE";
    case ErrorCode::kSchedDagTypeMismatch:
      return "SCHED_DAG_TYPE_MISMATCH";
    case ErrorCode::kSchedDagResourceInfeasible:
      return "SCHED_DAG_RESOURCE_INFEASIBLE";
    case ErrorCode::kSchedTaskUnknown:
      return "SCHED_TASK_UNKNOWN";
    case ErrorCode::kSchedPersistence:
      return "SCHED_PERSISTENCE";
    case ErrorCode::kSchedCacheMiss:
      return "SCHED_CACHE_MISS";
    case ErrorCode::kSchedCancelled:
      return "SCHED_CANCELLED";
    case ErrorCode::kWorkerProtocol:
      return "WORKER_PROTOCOL";
    case ErrorCode::kWorkerHeartbeatTimeout:
      return "WORKER_HEARTBEAT_TIMEOUT";
    case ErrorCode::kWorkerCrashed:
      return "WORKER_CRASHED";
    case ErrorCode::kWorkerTerminated:
      return "WORKER_TERMINATED";
    case ErrorCode::kWorkerBusy:
      return "WORKER_BUSY";
    case ErrorCode::kValidationSchema:
      return "VALIDATION_SCHEMA";
    case ErrorCode::kValidationDomain:
      return "VALIDATION_DOMAIN";
    case ErrorCode::kImportUnreadable:
      return "IMPORT_UNREADABLE";
    case ErrorCode::kImportCorrupt:
      return "IMPORT_CORRUPT";
    case ErrorCode::kImportUnsupportedFormat:
      return "IMPORT_UNSUPPORTED_FORMAT";
    case ErrorCode::kImportMissingExif:
      return "IMPORT_MISSING_EXIF";
    case ErrorCode::kImportSensorUnresolved:
      return "IMPORT_SENSOR_UNRESOLVED";
    case ErrorCode::kImportTimestampUnresolvable:
      return "IMPORT_TIMESTAMP_UNRESOLVABLE";
    case ErrorCode::kImportValidationError:
      return "IMPORT_VALIDATION_ERROR";
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
