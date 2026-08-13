#pragma once

// ProjectError hierarchy (ADR-014, docs/architecture/error-model.md).
//
// Stable, machine-readable error codes survive process boundaries; they are
// the contract between Core, the scheduler, workers, and the Python SDK.
// Numeric codes are never reused; string forms (e.g. "STORAGE_ATOMIC_WRITE")
// are carried in ErrorInfo.code across IPC (schemas/protobuf/errors.proto).

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace spatial::core {

enum class ErrorDomain : int32_t {
  kProject = 1,
  kStorage = 2,
  kSchema = 3,
  kCoordinate = 4,
  kCalibration = 5,
  kImport = 6,
  kArtifact = 7,
  kScheduler = 8,
  kWorker = 9,
  kAdapter = 10,
  kValidation = 11,
};

enum class ErrorCode : int32_t {
  // STORAGE_* (domain 2)
  kStorageCorrupt = 2001,
  kStorageReadOnly = 2002,
  kStorageLock = 2003,
  kStorageIo = 2004,
  kStorageAtomicWrite = 2005,
  // SCHEMA_* (domain 3)
  kSchemaVersion = 3001,
  kSchemaMigration = 3002,
  kSchemaInvalid = 3003,
  // ARTIFACT_* (domain 7)
  kArtifactHashMismatch = 7001,
  kArtifactMissing = 7002,
  kArtifactManifest = 7003,
  kArtifactGcConflict = 7004,
  kArtifactUnreferenced = 7005,
  // COORD_* (domain 4)
  kCoordFrameMismatch = 4001,
  kCoordFrameNotFound = 4002,
  kCoordFrameCycle = 4003,
  kCoordFrameDisconnected = 4004,
  kCoordFrameMultipleRoots = 4005,
  kCoordFrameExists = 4006,
  // SCHED_* (domain 8)
  kSchedDagCycle = 8001,
  kSchedDagTypeMismatch = 8002,
  kSchedDagResourceInfeasible = 8003,
  kSchedTaskUnknown = 8004,
  kSchedPersistence = 8005,
  kSchedCacheMiss = 8006,
  kSchedCancelled = 8007,
  // WORKER_* (domain 9)
  kWorkerProtocol = 9001,
  kWorkerHeartbeatTimeout = 9002,
  kWorkerCrashed = 9003,
  kWorkerTerminated = 9004,
  kWorkerBusy = 9005,
  // PROJECT_* (domain 1)
  kProjectInvalid = 1001,
  kProjectNotFound = 1002,
  kProjectCorrupt = 1003,
  // CALIBRATION_* (domain 5)
  kCalibrationInvalid = 5001,
  // VALIDATION_* (domain 11)
  kValidationSchema = 11001,
  kValidationDomain = 11002,
  // ADAPTER_* (domain 10) — backend process execution (C1-S3, RFC-0008 §10;
  // adding-adapter.md §6). Stable codes carried across the worker boundary.
  kAdapterProcessFailed = 10001,    // deterministic: non-zero exit / spawn
                                    // failure / input materialization failure
  kAdapterProcessTimeout = 10002,   // transient: stage exceeded its deadline
  kAdapterProcessCancelled = 10003, // cooperative stop at a tool boundary
  // IMPORT_* (domain 6) — RFC-0006 §6.6, image-import.md §12. Per-file import
  // failures; a failed file never partially writes.
  kImportUnreadable = 6001,
  kImportCorrupt = 6002,
  kImportUnsupportedFormat = 6003,
  kImportMissingExif = 6004,
  kImportSensorUnresolved = 6005,
  kImportTimestampUnresolvable = 6006,
  kImportValidationError = 6007,
  // INTERNAL
  kInternal = 0,
};

ErrorDomain DomainOf(ErrorCode code) noexcept;
const char* DomainName(ErrorDomain domain) noexcept;
const char* ErrorCodeName(ErrorCode code) noexcept;
std::string StableErrorCode(ErrorCode code);

struct ErrorContext {
  std::string key;
  std::string value;
};

class ProjectError : public std::exception {
 public:
  ProjectError(ErrorCode code, std::string message,
               std::vector<ErrorContext> context = {},
               bool recoverable = false, std::string suggested_action = "",
               std::shared_ptr<const ProjectError> cause = nullptr);

  ~ProjectError() override;

  const char* what() const noexcept override;

  ErrorCode code() const noexcept { return code_; }
  ErrorDomain domain() const noexcept { return DomainOf(code_); }
  const std::string& message() const noexcept { return message_; }
  const std::vector<ErrorContext>& context() const noexcept { return context_; }
  bool recoverable() const noexcept { return recoverable_; }
  const std::string& suggested_action() const noexcept {
    return suggested_action_;
  }
  const std::shared_ptr<const ProjectError>& cause() const noexcept {
    return cause_;
  }

 private:
  ErrorCode code_;
  std::string message_;
  std::vector<ErrorContext> context_;
  bool recoverable_;
  std::string suggested_action_;
  std::shared_ptr<const ProjectError> cause_;
  std::string what_;
};

class StorageError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class SchemaError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class CoordinateError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class CalibrationError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class ImportError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class ArtifactError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class SchedulerError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class WorkerError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class AdapterError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

class ValidationError : public ProjectError {
 public:
  using ProjectError::ProjectError;
};

}  // namespace spatial::core
