# Error Model

- **Status:** ratified (P0)
- **References:** ADR-014 (error handling), ADR-012 (IPC), ADR-018 (strict types)
- **Protected surface:** error codes under `schemas/**` (Constitution §2) — the error contract is part of the public API and evolves only through RFC

## 1. Why a typed hierarchy

The platform is multi-process: Core runs in the main process, algorithms run in isolated worker processes over Protobuf IPC. A bare exception type and message string cannot survive these boundaries and cannot drive the UI's "recoverable? suggested action?" behavior. Errors must be **machine-readable** so the SDK/CLI can react, the scheduler can classify retryability, and operators can correlate failures with logs.

## 2. Hierarchy

All errors derive from `ProjectError`. The family:

| Type | Meaning | Example code family |
|---|---|---|
| `ProjectError` | base of all platform errors | `PROJECT_*` |
| `StorageError` | project open/create/save, locking, I/O, atomicity | `STORAGE_*` |
| `SchemaError` | schema version mismatch, migration failure, invalid records | `SCHEMA_*` |
| `CoordinateError` | frame mismatch, unit confusion, invalid transform | `COORD_*` |
| `CalibrationError` | invalid/missing/conflicting calibration | `CALIB_*` |
| `ImportError` | ingestion failure, unreadable/corrupt source files | `IMPORT_*` |
| `ArtifactError` | hash mismatch, missing payload, manifest corruption, GC conflict | `ARTIFACT_*` |
| `SchedulerError` | DAG validation, state persistence, retry exhaustion | `SCHED_*` |
| `WorkerError` | worker crash, protocol violation, heartbeat timeout | `WORKER_*` |
| `AdapterError` | backend failure, capability mismatch, license gate | `ADAPTER_*` |
| `ValidationError` | input fails schema/domain validation | `VALID*` |

The hierarchy lets callers handle whole families uniformly (any `StorageError`), while stable codes give fine-grained reaction.

## 3. Every error carries

1. **Stable code** — numeric, never reused; declared in `schemas/**`; adding or changing a code requires a ratified RFC. CI validates codes against the schema.
2. **Message** — human-readable summary.
3. **Technical context** — structured key/value map (worker_id, artifact hash, task id, frame pair, ...).
4. **`recoverable` flag** — drives scheduler retry (transient → retry with backoff; deterministic → fail with reproducible report).
5. **Suggested action** — user-facing remediation for the UI.
6. **Chained cause** — the underlying error is always preserved; boundary translation tests assert cause preservation.

## 4. Boundaries: exceptions never cross IPC

- **No C++ exceptions cross the scheduler/worker interface** or any Protobuf frame. At every IPC boundary, exceptions are translated into structured error payloads carrying the stable code, message, and context (mapped to the worker-protocol error codes: `WorkerError`, `AdapterError`, `ValidationError`).
- **Within a process**, exceptions remain the mechanism for genuine programming errors; internal Core boundaries that are contractually guaranteed are `noexcept` (e.g. observers, cancellation checks).
- Misuse of `noexcept` is mitigated by mandatory cause chaining and boundary tests.

## 5. C++ sketch

```cpp
// core/errors/project_error.h
namespace spatial::core {

enum class ErrorCode : int32_t {           // schema-derived; see schemas/error-codes.json
  kProjectStorageCorrupt  = 1001,
  kProjectReadOnly        = 1002,
  kArtifactHashMismatch   = 3001,
  kCoordFrameMismatch     = 4001,
  kWorkerLost             = 8001,
  kAdapterCapability      = 9001,
  kValidationSchema       = 10001,
  // ...
};

struct ErrorContext {                      // structured key/value
  std::string key;
  std::string value;
};

class ProjectError : public std::exception {
 public:
  ProjectError(ErrorCode code, std::string message,
               std::vector<ErrorContext> context = {},
               bool recoverable = false,
               std::string suggested_action = "",
               std::shared_ptr<const ProjectError> cause = nullptr);

  const char* what() const noexcept override;
  ErrorCode code() const noexcept;             // stable, schema-declared
  const std::string& message() const noexcept;
  const std::vector<ErrorContext>& context() const noexcept;
  bool recoverable() const noexcept;
  const std::string& suggested_action() const noexcept;
  const std::shared_ptr<const ProjectError>& cause() const noexcept;

 private:
  ErrorCode code_;
  std::string message_;
  std::vector<ErrorContext> context_;
  bool recoverable_;
  std::string suggested_action_;
  std::shared_ptr<const ProjectError> cause_;
};

class StorageError    : public ProjectError { /* ctor forwards + family code */ };
class SchemaError     : public ProjectError { /* ... */ };
class CoordinateError : public ProjectError { /* ... */ };
class CalibrationError: public ProjectError { /* ... */ };
class ImportError     : public ProjectError { /* ... */ };
class ArtifactError   : public ProjectError { /* ... */ };
class SchedulerError  : public ProjectError { /* ... */ };
class WorkerError     : public ProjectError { /* ... */ };
class AdapterError    : public ProjectError { /* ... */ };
class ValidationError : public ProjectError { /* ... */ };

}  // namespace spatial::core
```

### 5.1 Translation to the worker protocol

```cpp
// At the IPC boundary only.
WorkerErrorPayload to_worker_payload(const ProjectError& e) {
  WorkerErrorPayload out;
  out.set_code(static_cast<int32_t>(e.code()));       // stable code, not text
  out.set_message(e.message());
  for (const auto& kv : e.context()) { auto* c = out.add_context();
    c->set_key(kv.key); c->set_value(kv.value); }
  out.set_recoverable(e.recoverable());
  out.set_suggested_action(e.suggested_action());
  // cause: either embedded, or referenced by log correlation id
  return out;
}
```

Worker-protocol error payloads use only the typed codes from §2 (`WorkerError`, `AdapterError`, `ValidationError`), never raw exception text.

## 6. Consistency rules

- Validation, adapters, storage, and the scheduler all use the same hierarchy so retry/resume logic relies on `recoverable` + code, never on message text.
- The scheduler retries **only** recoverable failures (ADR-020); logs and worker crashes correlate through stable codes (ADR-015).
- Code lists are **generated from `schemas/**`** to prevent drift between C++ and Python.

## 7. Testing

- Unit tests: every error carries all six fields; cause preservation on translation; `noexcept` boundary behavior.
- Schema validation at CI time checks codes against `schemas/error-codes.json`.
- Boundary tests assert the worker protocol carries the stable code, context, and recoverable flag losslessly.

## References

- `docs/architecture/process-model.md`, `docs/architecture/storage-model.md`
- `docs/specifications/error-model.md`
- ADR-014, ADR-012, ADR-020
