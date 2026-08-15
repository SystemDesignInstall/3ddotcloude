#pragma once

// Real M0 worker backend (RFC-0003 §5.7, ADR-011/012): spawns a worker child
// process (the demo Python worker), negotiates WorkerHello with capabilities,
// and drives tasks over the framed protobuf protocol. Supervises liveness
// (heartbeat timeout / EOF = crash), delivers cooperative cancellation, and
// shuts the worker down cleanly.
//
// C1-S1 (RFC-0008/0009): the executor owns the worker boundary of the CAS
// contract. When an ArtifactStore is injected it
//   * materializes each TaskRequest.input_refs hash into
//     workspace/inputs/<hash> before dispatch (CAS lookup -> file), and
//   * performs fail-closed CAS ingest of produced payloads: verifies
//     payload_path exists, SHA-256(payload) == content_hash, parses
//     manifest_json, then ArtifactStore::Put — and only then reports
//     kArtifactProduced. Any materialization or ingest failure surfaces as
//     kFailed (never a silent no-op). Without a store, inputs cannot be
//     materialized (a task with non-empty input_refs fails closed) and
//     produced payloads are passed through as before.
//
// Not thread-safe: the scheduler is the single owner (process-model §5).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "engine/workers/child_process.h"
#include "engine/workers/worker_handle.h"
#include "generated/worker.pb.h"

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
  // `store` (optional) enables input materialization + fail-closed CAS
  // ingest (C1-S1); it must outlive this executor.
  ProcessExecutor(ResourceProfile fallback_profile,
                  std::vector<std::string> worker_command,
                  std::string proto_dir,
                  std::int64_t heartbeat_timeout_ms = 5000,
                  spatial::core::ArtifactStore* store = nullptr);

  ~ProcessExecutor() override;

  const ResourceProfile& profile() const override;
  Uuid id() const override;
  std::string implementation_label() const override;
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

  // Materializes `request.input_refs` into `<workspace>/inputs/<hash>` via
  // CAS lookup. Returns false with `error` on any failure (no silent no-op):
  // no store configured, unknown hash, corrupt payload, or write error.
  bool MaterializeInputs(const TaskRequest& request, std::string& error);

  // Fail-closed CAS ingest of a produced artifact (C1-S1). Returns false with
  // `error` on: missing payload_path, missing payload file, SHA-256 mismatch,
  // missing/malformed manifest, manifest hash mismatch, or CAS write failure.
  bool IngestArtifact(const spatial::ArtifactInfo& artifact,
                      std::string& error);

  // Terminates the worker process after an unrecoverable protocol-level
  // failure (a corrupt artifact) so no further frames corrupt the stream.
  void TerminateWorker();

  std::unique_ptr<ChildProcess> child_;
  ResourceProfile profile_;
  Uuid id_;
  std::string worker_id_;
  int protocol_version_ = 0;
  std::int64_t heartbeat_timeout_ms_;
  spatial::core::ArtifactStore* store_ = nullptr;

  bool running_ = false;         // a task is in flight on the worker
  bool cancel_pending_ = false;  // TaskCancelled sent, awaiting ack
  std::string task_id_;          // canonical task id string under execution
  Uuid task_uuid_{};             // task id under execution
  bool shutdown_ = false;

  // Fail-closed state (C1-S1): set by Submit when input materialization
  // failed. WaitForEvent surfaces it as kFailed before touching the worker.
  bool pending_failure_ = false;
  std::string pending_failure_code_;
  std::string pending_failure_message_;
};

}  // namespace spatial::engine
