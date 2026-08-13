#pragma once

// ProcessingAdapter (RFC-0007 §7 placeholder activated by RFC-0008 §5; C1
// plan §3.1, plugin-api.md §2). The worker-side adapter seam: an adapter is
// consumed inside its worker process to validate its environment, plan one
// worker task, and execute the plan. The engine consumes only WorkerExecutor
// (ADR-034); no backend type or include escapes the adapter layer (RFC-0008
// §17, check_arch_debt). The adapter never writes Core, Scene, or CAS
// structures directly from the backend — conversion happens inside the adapter
// (principles 9, 15). Revision 3 removed WorkerCommand() from the seam:
// process spawning is the worker's concern; the adapter never constructs
// worker argv.
//
// Errors surface as typed spatial::core::ProjectError subtypes (ADR-014:
// AdapterError / ValidationError with a stable code, recoverable flag and
// suggested action). Exceptions never cross the process boundary; the worker
// translates them into the framed worker protocol (adding-adapter.md §6).
//
// This header is Constitution-protected (adapters/interfaces/**, CONSTITUTION
// §2). It depends only on std types plus the engine's header-only protocol
// value types (TaskRequest / ResourceProfile); it never links the engine.

#include <string>
#include <vector>

#include "adapters/interfaces/adapter_descriptor.h"
#include "engine/workers/worker_handle.h"

namespace spatial::adapters {

// Worker-side sink an adapter emits into during execution; the worker process
// bridges these calls to the framed worker protocol (TaskProgress / TaskLog /
// TaskArtifactProduced). The adapter never touches the wire itself.
class ResultSink {
 public:
  virtual ~ResultSink() = default;

  // Substage progress in percent 0..100 (worker-protocol TaskProgress, one
  // substage per CLI tool, C1 plan §3.2).
  virtual void Progress(int percent, const std::string& stage) = 0;

  // Log line (worker-protocol TaskLog).
  virtual void Log(const std::string& message) = 0;

  // One canonical artifact produced. `payload_path` is the on-disk payload
  // and `manifest_json` the ArtifactManifest document; the host verifies and
  // ingests them into the CAS (C1-S1 fail-closed ingest contract).
  virtual void ArtifactProduced(const std::string& payload_path,
                                const std::string& manifest_json) = 0;
};

// One task's worth of adapter work. Implementations are shared by a worker
// process and must be safe to call from that process only (never from the
// engine — the engine sees only WorkerExecutor).
class ProcessingAdapter {
 public:
  virtual ~ProcessingAdapter() = default;

  // Static identity + capability declaration (plugin-api.md §3).
  virtual AdapterDescriptor Descriptor() const = 0;

  // Doctor step (adding-adapter.md §5): true when the backend is runnable
  // here; otherwise false with a human-readable `problem`. Availability is
  // gated here (missing executable -> diagnostic, never a hard failure at
  // dispatch).
  virtual bool ValidateEnvironment(std::string& problem) const = 0;

  // Build the execution plan for one worker task from the request's inputs
  // and effective configuration (adding-adapter.md §6). Planning is pure
  // reasoning over inputs — it never runs the backend. Throws a
  // spatial::core::ProjectError subtype on a contract-violating request
  // (e.g. a calibration value in config_json, RFC-0009 §6).
  virtual std::vector<std::string> CreatePlan(
      const spatial::engine::TaskRequest& request) const = 0;

  // Execute `plan` inside the worker process, emitting canonical artifacts
  // and progress into `sink`. Throws a ProjectError subtype on a
  // determinable failure (no exceptions cross the process boundary).
  virtual void Execute(const std::vector<std::string>& plan,
                       ResultSink& sink) = 0;
};

}  // namespace spatial::adapters
