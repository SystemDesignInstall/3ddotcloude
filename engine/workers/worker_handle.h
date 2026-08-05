#pragma once

// Worker boundary (RFC-0003 §5.7, engine.md §6, scheduler-design §8). This
// header fixes the contract between the scheduler and a worker. Concrete
// executors (InProcessExecutor, ProcessExecutor) ship with the workers
// milestone (P1.3); the interface is defined here so the scheduler and its
// unit tests do not depend on the IPC implementation (ADR-021).

#include <cstdint>
#include <string>
#include <vector>

#include "engine/engine_common.h"
#include "engine/resources/resource_spec.h"
#include "engine/task/task_types.h"

namespace spatial::engine {

// What the scheduler sends to a worker (worker-protocol TaskRequest).
struct TaskRequest {
  Uuid task_id{};
  std::string task_type;
  std::vector<ArtifactRef> input_refs;         // CAS content hashes
  std::vector<ArtifactRef> expected_output_refs;
  std::string config_json;                     // effective configuration
  std::string workspace;                       // deterministic temp/<job>/<task>
};

// Events a worker emits back to the scheduler (worker-protocol message set).
enum class WorkerEventType : int {
  kProgress = 0,           // progress = percent 0..100
  kLog = 1,                // log line
  kArtifactProduced = 2,   // artifact_ref produced
  kCompleted = 3,          // terminal success
  kFailed = 4,             // terminal failure (error code/message/recoverable)
  kCancelled = 5,          // terminal cancellation
  kHeartbeat = 6,          // liveness
};

struct WorkerEvent {
  WorkerEventType type = WorkerEventType::kHeartbeat;
  Uuid task_id{};
  int progress = 0;
  std::string log;
  ArtifactRef artifact_ref;
  std::string error_code;    // stable string, e.g. "WORKER_CRASHED"
  std::string error_message;
  bool recoverable = false;
};

// Executor contract implemented by ProcessExecutor (real, P1.3) and
// InProcessExecutor (mock, ADR-021). The scheduler is the single allocator:
// workers never claim resources on their own (process-model §5).
class WorkerExecutor {
 public:
  virtual ~WorkerExecutor() = default;

  virtual const ResourceProfile& profile() const = 0;

  // Stable identity recorded in ExecutionRecord.worker_id.
  virtual Uuid id() const = 0;

  // Starts a task asynchronously. Throws WorkerError WORKER_BUSY when the
  // worker cannot accept another task right now.
  virtual void Submit(const TaskRequest& request) = 0;

  // Cooperative cancellation (worker-protocol TaskCancelled).
  virtual void Cancel(const Uuid& task_id, const std::string& reason) = 0;

  // Blocks until a worker event arrives or `timeout_ms` elapses. Returns
  // false on timeout.
  virtual bool WaitForEvent(WorkerEvent& out, std::int64_t timeout_ms) = 0;

  // Graceful teardown.
  virtual void Shutdown() = 0;
};

}  // namespace spatial::engine
