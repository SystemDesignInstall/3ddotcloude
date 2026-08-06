#include "engine/workers/process_executor.h"

#include <utility>
#include <vector>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/workers/protocol_framing.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::ParseUuid;
using spatial::core::WorkerError;
using spatial::core::fs::TimestampNsNow;

constexpr std::int64_t kHandshakeTimeoutMs = 10000;
constexpr std::int64_t kShutdownWaitMs = 5000;

}  // namespace

ProcessExecutor::ProcessExecutor(ResourceProfile fallback_profile,
                                 std::vector<std::string> worker_command,
                                 std::string proto_dir,
                                 std::int64_t heartbeat_timeout_ms)
    : profile_(std::move(fallback_profile)),
      heartbeat_timeout_ms_(heartbeat_timeout_ms) {
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

void ProcessExecutor::Submit(const TaskRequest& request) {
  if (shutdown_ || child_ == nullptr) {
    throw WorkerError(ErrorCode::kWorkerTerminated,
                      "worker is shut down");
  }
  if (running_) {
    throw WorkerError(ErrorCode::kWorkerBusy,
                      "worker already has a task in flight (max_concurrency = 1)");
  }
  spatial::WorkerMessage msg;
  auto* req = msg.mutable_task_request();
  req->set_task_id(FormatUuid(request.task_id));
  req->set_task_type(request.task_type);
  req->set_spec_json(request.config_json);
  req->set_workspace(request.workspace);

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
