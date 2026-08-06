#pragma once

// Real M0 worker backend (RFC-0003 §5.7, ADR-011/012): spawns a worker child
// process (the demo Python worker), negotiates WorkerHello with capabilities,
// and drives tasks over the framed protobuf protocol. Supervises liveness
// (heartbeat timeout / EOF = crash), delivers cooperative cancellation, and
// shuts the worker down cleanly.
//
// Not thread-safe: the scheduler is the single owner (process-model §5).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/workers/child_process.h"
#include "engine/workers/worker_handle.h"

namespace spatial::engine {

class ProcessExecutor : public WorkerExecutor {
 public:
  // `fallback_profile` seeds the worker identity until WorkerHello arrives;
  // the hello's capabilities/resources/max_concurrency override it.
  // `worker_command` is the full argv of the worker process (e.g.
  // {"python", "/abs/demo_worker.py"}). `proto_dir` is added to the child's
  // PYTHONPATH so the worker can import the generated protobuf module. The
  // constructor spawns the worker and performs the handshake; it throws
  // WorkerError (WORKER_PROTOCOL) on spawn failure or version mismatch.
  ProcessExecutor(ResourceProfile fallback_profile,
                  std::vector<std::string> worker_command,
                  std::string proto_dir,
                  std::int64_t heartbeat_timeout_ms = 5000);

  ~ProcessExecutor() override;

  const ResourceProfile& profile() const override;
  Uuid id() const override;
  void Submit(const TaskRequest& request) override;
  void Cancel(const Uuid& task_id, const std::string& reason) override;
  bool WaitForEvent(WorkerEvent& out, std::int64_t timeout_ms) override;
  void Shutdown() override;

 private:
  // Reads one frame and translates it into `out`. Returns false on timeout;
  // `crash` is set when the worker died or the stream ended (mapped to
  // WORKER_CRASHED).
  bool NextEvent(WorkerEvent& out, std::int64_t timeout_ms, bool& crash);

  void MarkCrashed(WorkerEvent& out, const Uuid& task_id,
                   const std::string& detail);

  std::unique_ptr<ChildProcess> child_;
  ResourceProfile profile_;
  Uuid id_;
  std::string worker_id_;
  int protocol_version_ = 0;
  std::int64_t heartbeat_timeout_ms_;

  bool running_ = false;         // a task is in flight on the worker
  bool cancel_pending_ = false;  // TaskCancelled sent, awaiting ack
  std::string task_id_;          // canonical task id string under execution
  Uuid task_uuid_{};             // task id under execution
  bool shutdown_ = false;
};

}  // namespace spatial::engine
