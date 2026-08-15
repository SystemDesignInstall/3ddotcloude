#pragma once

// Mock worker behind the WorkerHandle contract (RFC-0003 §5.7, ADR-021).
// InProcessExecutor is a test-only execution path: it runs task bodies on a
// background thread inside the engine process and streams WorkerEvents to the
// scheduler through the same interface as a real worker. It is NOT a
// production execution path (ADR-011: process isolation).

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "engine/workers/worker_handle.h"

namespace spatial::engine {

// A task body executed inside the in-process worker. `emit` is thread-safe
// and pushes a WorkerEvent to the scheduler; `cancelled` returns true once
// the scheduler asked for cooperative cancellation (the body should then emit
// kCancelled and return).
using InProcessTaskRunner = std::function<void(
    const TaskRequest&, std::function<void(WorkerEvent)> emit,
    std::function<bool()> cancelled)>;

// Runs one task at a time (max_concurrency = 1).
class InProcessExecutor : public WorkerExecutor {
 public:
  // `runner` is the task body; when empty the default mock is used: it writes
  // a deterministic payload into the task workspace, emits kArtifactProduced
  // with a content hash over the task inputs, then kCompleted.
  InProcessExecutor(ResourceProfile profile, InProcessTaskRunner runner = {});

  ~InProcessExecutor() override;

  const ResourceProfile& profile() const override;
  Uuid id() const override;
  std::string implementation_label() const override;
  void Submit(const TaskRequest& request) override;
  void Cancel(const Uuid& task_id, const std::string& reason) override;
  bool WaitForEvent(WorkerEvent& out, std::int64_t timeout_ms) override;
  void Shutdown() override;

 private:
  void RunTask(TaskRequest request);

  ResourceProfile profile_;
  Uuid id_;
  InProcessTaskRunner runner_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<WorkerEvent> events_;
  bool busy_ = false;              // a task is executing
  Uuid running_task_{};            // task under execution
  bool cancelled_ = false;         // cooperative cancel requested
  std::string cancel_reason_;
  bool shutdown_ = false;
  std::thread worker_thread_;
};

// Deterministic mock artifact: content hash over the task identity and inputs
// (ADR-020 style). Used by the default runner and by scheduler tests.
std::string MockArtifactHash(const TaskRequest& request);

}  // namespace spatial::engine
