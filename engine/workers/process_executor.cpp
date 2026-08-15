#include "engine/workers/process_executor.h"

#include <filesystem>
#include <utility>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"
#include "engine/workers/protocol_framing.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::ParseUuid;
using spatial::core::Sha256Hex;
using spatial::core::WorkerError;
using spatial::core::fs::TimestampNsNow;

constexpr std::int64_t kHandshakeTimeoutMs = 10000;
constexpr std::int64_t kShutdownWaitMs = 5000;

}  // namespace

ProcessExecutor::ProcessExecutor(ResourceProfile fallback_profile,
                                 std::vector<std::string> worker_command,
                                 std::string proto_dir,
                                 std::int64_t heartbeat_timeout_ms,
                                 spatial::core::ArtifactStore* store)
    : profile_(std::move(fallback_profile)),
      heartbeat_timeout_ms_(heartbeat_timeout_ms),
      store_(store) {
  std::string error;
  std::vector<std::string> env;
  if (!proto_dir.empty()) {
    env.push_back("PYTHONPATH=" + proto_dir);
  }
  child_ = ChildProcess::Spawn(std::move(worker_command), error, env);
  if (child_ == nullptr) {
    throw WorkerError(ErrorCode::kWorkerProtocol,
                      "failed to spawn worker: " + error);
  }

  // Handshake: the first frame must be WorkerHello with a compatible version.
  spatial::WorkerMessage hello_msg;
  bool hello_seen = false;
  const auto deadline = TimestampNsNow() + kHandshakeTimeoutMs * 1000000LL;
  while (!hello_seen) {
    const std::int64_t remaining_ns = deadline - TimestampNsNow();
    if (remaining_ns <= 0) {
      throw WorkerError(ErrorCode::kWorkerProtocol,
                        "worker did not send WorkerHello within the timeout");
    }
    std::string frame;
    bool eof = false;
    if (!ReadFrame(*child_, remaining_ns / 1000000LL, frame, eof, error)) {
      if (eof) {
        throw WorkerError(ErrorCode::kWorkerProtocol,
                          "worker exited before WorkerHello: " + error);
      }
      continue;
    }
    if (!TryParseFrame(frame, hello_msg)) {
      throw WorkerError(ErrorCode::kWorkerProtocol,
                        "worker sent a malformed frame during handshake");
    }
    if (hello_msg.has_hello()) {
      const auto& hello = hello_msg.hello();
      protocol_version_ = hello.protocol_version();
      if (protocol_version_ != 1) {
        throw WorkerError(ErrorCode::kWorkerProtocol,
                          "worker protocol version " +
                              std::to_string(protocol_version_) +
                              " is incompatible with engine version " +
                              kWorkerProtocolVersion);
      }
      worker_id_ = hello.worker_id();
      if (!worker_id_.empty()) {
        try {
          id_ = ParseUuid(worker_id_);
        } catch (...) {
          id_ = GenerateUuid();
        }
      } else {
        id_ = GenerateUuid();
      }
      const auto& caps = hello.capabilities();
      profile_.capabilities.assign(caps.capabilities().begin(),
                                   caps.capabilities().end());
      profile_.max_concurrency =
          caps.max_concurrency() > 0 ? caps.max_concurrency() : 1;
      profile_.capacity.cores =
          static_cast<int>(caps.resources().cpu_cores() > 0
                               ? caps.resources().cpu_cores()
                               : 1);
      profile_.capacity.ram_bytes =
          static_cast<std::int64_t>(caps.resources().ram_mb()) * 1048576LL;
      profile_.capacity.gpu_mem_bytes =
          static_cast<std::int64_t>(caps.resources().gpu_mem_mb()) * 1048576LL;
      hello_seen = true;
    }
    // Any other first message is ignored until the hello arrives.
  }
}

ProcessExecutor::~ProcessExecutor() {
  if (child_ != nullptr) {
    Shutdown();
  }
}

const ResourceProfile& ProcessExecutor::profile() const { return profile_; }

Uuid ProcessExecutor::id() const { return id_; }

std::string ProcessExecutor::implementation_label() const {
  return "process";
}

void ProcessExecutor::Submit(const TaskRequest& request) {
  if (shutdown_ || child_ == nullptr) {
    throw WorkerError(ErrorCode::kWorkerTerminated,
                      "worker is shut down");
  }
  if (running_) {
    throw WorkerError(ErrorCode::kWorkerBusy,
                      "worker already has a task in flight (max_concurrency = 1)");
  }

  // C1-S1: materialize every declared input into the worker's workspace
  // BEFORE dispatch (TaskRequest.input_refs -> CAS lookup ->
  // workspace/inputs/<hash> -> worker). Fail-closed: any failure records a
  // pending kFailed instead of dispatching a worker that would run with
  // missing inputs (no silent no-op).
  if (!request.input_refs.empty()) {
    std::string materialize_error;
    if (!MaterializeInputs(request, materialize_error)) {
      pending_failure_ = true;
      pending_failure_code_ = "WORKER_INPUT_MATERIALIZE";
      pending_failure_message_ = std::move(materialize_error);
      running_ = true;  // WaitForEvent surfaces the terminal kFailed
      task_id_ = FormatUuid(request.task_id);
      task_uuid_ = request.task_id;
      return;
    }
  }

  spatial::WorkerMessage msg;
  auto* req = msg.mutable_task_request();
  req->set_task_id(FormatUuid(request.task_id));
  req->set_task_type(request.task_type);
  req->set_spec_json(request.config_json);
  req->set_workspace(request.workspace);
  for (const auto& ref : request.input_refs) {
    req->add_input_refs(ref);
  }

  std::string error;
  // A broken pipe here means the worker already died; the dispatch is still
  // recorded and the crash surfaces as WORKER_CRASHED from WaitForEvent
  // (worker-protocol §5: exit / closed stdout => interrupted).
  (void)WriteFrame(*child_, msg, error);
  running_ = true;
  cancel_pending_ = false;
  task_id_ = FormatUuid(request.task_id);
  task_uuid_ = request.task_id;
}

bool ProcessExecutor::MaterializeInputs(const TaskRequest& request,
                                        std::string& error) {
  if (store_ == nullptr) {
    error = "input materialization requires an artifact store (none configured)";
    return false;
  }
  const auto inputs_dir =
      std::filesystem::path(request.workspace) / "inputs";
  try {
    spatial::core::fs::CreateDirectories(inputs_dir);
  } catch (...) {
    error = "cannot create inputs directory: " + inputs_dir.string();
    return false;
  }
  for (const auto& ref : request.input_refs) {
    // CAS lookup; Get() re-verifies SHA-256 and throws on a corrupt payload.
    std::optional<std::vector<std::uint8_t>> bytes;
    try {
      bytes = store_->Get(ref);
    } catch (...) {
      error = "input corrupt in CAS: " + ref;
      return false;
    }
    if (!bytes) {
      error = "input not in CAS: " + ref;
      return false;
    }
    const auto target = inputs_dir / ref;
    try {
      spatial::core::fs::AtomicWrite(target, *bytes);
    } catch (...) {
      error = "cannot materialize input " + ref + ": " + target.string();
      return false;
    }
  }
  return true;
}

bool ProcessExecutor::IngestArtifact(const spatial::ArtifactInfo& artifact,
                                     std::string& error) {
  if (store_ == nullptr) {
    error = "CAS ingest requires an artifact store (none configured)";
    return false;
  }
  if (artifact.payload_path().empty()) {
    error = "worker produced an artifact without a payload_path";
    return false;
  }
  const std::filesystem::path payload_path(artifact.payload_path());
  if (!spatial::core::fs::Exists(payload_path)) {
    error = "artifact payload not found: " + payload_path.string();
    return false;
  }
  std::vector<std::uint8_t> bytes;
  try {
    bytes = spatial::core::fs::ReadFile(payload_path);
  } catch (...) {
    error = "cannot read artifact payload: " + payload_path.string();
    return false;
  }
  if (Sha256Hex(bytes) != artifact.content_hash()) {
    error = "artifact payload SHA-256 does not match declared content_hash";
    return false;
  }
  if (artifact.manifest_json().empty()) {
    error = "worker produced an artifact without a manifest";
    return false;
  }
  ArtifactManifest manifest;
  try {
    manifest = spatial::core::FromJsonString(artifact.manifest_json());
  } catch (...) {
    error = "artifact manifest missing or malformed";
    return false;
  }
  if (manifest.content_hash != artifact.content_hash()) {
    error = "artifact manifest content_hash does not match declared hash";
    return false;
  }
  try {
    store_->Put(bytes, manifest);
  } catch (...) {
    error = "CAS ingest failed for artifact " + artifact.artifact_id();
    return false;
  }
  return true;
}

void ProcessExecutor::TerminateWorker() {
  if (child_ != nullptr && child_->IsRunning()) {
    child_->Terminate();
  }
  child_->Wait();
  child_.reset();
  running_ = false;
  cancel_pending_ = false;
}

void ProcessExecutor::Cancel(const Uuid& task_id, const std::string& reason) {
  if (!running_ || FormatUuid(task_id) != task_id_ || child_ == nullptr) {
    return;
  }
  spatial::WorkerMessage msg;
  auto* cancel = msg.mutable_task_cancelled();
  cancel->set_task_id(task_id_);
  cancel->set_reason(reason);
  std::string error;
  if (WriteFrame(*child_, msg, error)) {
    cancel_pending_ = true;
  }
}

bool ProcessExecutor::NextEvent(WorkerEvent& out, std::int64_t timeout_ms,
                                bool& crash) {
  crash = false;
  std::string frame;
  bool eof = false;
  std::string error;
  if (!ReadFrame(*child_, timeout_ms, frame, eof, error)) {
    if (eof) {
      crash = true;  // stdout closed: worker exited (crashed or finished)
    }
    return false;
  }

  spatial::WorkerMessage msg;
  if (!TryParseFrame(frame, msg)) {
    crash = true;  // malformed frame is a protocol error that terminates the
    return false;  // worker (worker-protocol §1)
  }

  if (msg.has_task_progress()) {
    const auto& p = msg.task_progress();
    out.task_id = task_uuid_;
    out.type = WorkerEventType::kProgress;
    out.progress = p.percent();
    if (cancel_pending_ && p.substage() == "cancelled") {
      out.type = WorkerEventType::kCancelled;
      running_ = false;
      cancel_pending_ = false;
    }
    return true;
  }
  if (msg.has_task_log()) {
    const auto& log = msg.task_log();
    out.task_id = task_uuid_;
    out.type = WorkerEventType::kLog;
    out.log = log.message();
    return true;
  }
  if (msg.has_task_artifact()) {
    const auto& artifact = msg.task_artifact().artifact();
    out.task_id = task_uuid_;
    out.payload_path = artifact.payload_path();
    out.manifest_json = artifact.manifest_json();
    // Fail-closed CAS ingest (C1-S1): the produced artifact reaches the
    // scheduler only after its payload was verified (SHA-256), its manifest
    // parsed, and the payload written to CAS. Any failure is a terminal
    // kFailed, never a silent no-op.
    if (store_ != nullptr) {
      std::string ingest_error;
      if (!IngestArtifact(artifact, ingest_error)) {
        out.type = WorkerEventType::kFailed;
        out.error_code = "WORKER_INGEST_FAILED";
        out.error_message = std::move(ingest_error);
        out.recoverable = false;
        running_ = false;
        cancel_pending_ = false;
        // The worker keeps producing frames for a task the scheduler now
        // considers terminal; terminate it so stale frames cannot corrupt a
        // later task's stream.
        TerminateWorker();
        return true;
      }
    }
    out.type = WorkerEventType::kArtifactProduced;
    out.artifact_ref = artifact.content_hash();
    return true;
  }
  if (msg.has_task_completed()) {
    const auto& completed = msg.task_completed();
    out.task_id = task_uuid_;
    out.type = WorkerEventType::kCompleted;
    if (cancel_pending_ && completed.outputs_size() == 0) {
      // The worker acknowledged the cooperative cancellation with an empty
      // completion; report the terminal cancellation state (RFC-0003 §5.6).
      out.type = WorkerEventType::kCancelled;
    }
    running_ = false;
    cancel_pending_ = false;
    return true;
  }
  if (msg.has_task_failed()) {
    const auto& error_info = msg.task_failed().error();
    out.task_id = task_uuid_;
    out.type = WorkerEventType::kFailed;
    out.error_code = error_info.code();
    if (out.error_code.empty()) {
      out.error_code = "WORKER_PROTOCOL";
    }
    out.error_message = error_info.message();
    out.recoverable = error_info.recoverable();
    running_ = false;
    cancel_pending_ = false;
    return true;
  }
  if (msg.has_heartbeat() || msg.has_task_accepted() || msg.has_hello() ||
      msg.has_shutdown()) {
    out.task_id = task_uuid_;
    out.type = WorkerEventType::kHeartbeat;
    return true;
  }
  // Unknown payload (oneof not set) is ignored and logged (worker-protocol §3).
  out.task_id = task_uuid_;
  out.type = WorkerEventType::kHeartbeat;
  return true;
}

bool ProcessExecutor::WaitForEvent(WorkerEvent& out, std::int64_t timeout_ms) {
  if (shutdown_ || child_ == nullptr) {
    return false;
  }
  // A fail-closed materialization failure from Submit surfaces here as the
  // task's terminal kFailed (the worker was never handed the request).
  if (pending_failure_) {
    pending_failure_ = false;
    out.task_id = task_uuid_;
    out.type = WorkerEventType::kFailed;
    out.error_code = pending_failure_code_;
    out.error_message = pending_failure_message_;
    out.recoverable = false;
    running_ = false;
    cancel_pending_ = false;
    return true;
  }
  bool crash = false;
  const bool got = NextEvent(out, timeout_ms, crash);
  if (crash) {
    MarkCrashed(out, task_uuid_, "worker process exited");
    running_ = false;
    cancel_pending_ = false;
    return true;
  }
  return got;
}

void ProcessExecutor::MarkCrashed(WorkerEvent& out, const Uuid& task_id,
                                  const std::string& detail) {
  out.task_id = task_id;
  out.type = WorkerEventType::kFailed;
  out.error_code = "WORKER_CRASHED";
  out.error_message = detail;
  out.recoverable = true;  // a crashed worker triggers the retry policy
}

void ProcessExecutor::Shutdown() {
  if (child_ == nullptr) {
    return;
  }
  if (!shutdown_) {
    shutdown_ = true;
    spatial::WorkerMessage msg;
    msg.mutable_shutdown()->set_reason("engine shutdown");
    std::string error;
    WriteFrame(*child_, msg, error);
    // Give the worker a moment to exit cleanly, then force-terminate.
    const auto deadline = TimestampNsNow() + kShutdownWaitMs * 1000000LL;
    while (child_->IsRunning() && TimestampNsNow() < deadline) {
      std::string frame;
      bool eof = false;
      ReadFrame(*child_, 50, frame, eof, error);
      if (eof) {
        break;
      }
    }
    if (child_->IsRunning()) {
      child_->Terminate();
    }
  }
  child_->Wait();
  child_.reset();
}

}  // namespace spatial::engine
