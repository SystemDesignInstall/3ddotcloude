#include "engine/workers/in_process_executor.h"

#include <algorithm>
#include <utility>

#include "core/errors/project_error.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"
#include "engine/workers/worker_handle.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::Sha256Hex;
using spatial::core::WorkerError;

std::string DefaultMockRunnerPayload(const TaskRequest& request) {
  std::string content = request.task_type + "|" + request.config_json;
  for (const auto& ref : request.input_refs) {
    content += "|" + ref;
  }
  return content;
}

}  // namespace

std::string MockArtifactHash(const TaskRequest& request) {
  return Sha256Hex(DefaultMockRunnerPayload(request));
}

void DefaultInProcessRunner(const TaskRequest& request,
                            const std::function<void(WorkerEvent)>& emit,
                            const std::function<bool()>& cancelled) {
  for (int p = 0; p <= 100; p += 25) {
    if (cancelled()) {
      WorkerEvent e;
      e.type = WorkerEventType::kCancelled;
      e.task_id = request.task_id;
      emit(e);
      return;
    }
    WorkerEvent progress;
    progress.type = WorkerEventType::kProgress;
    progress.task_id = request.task_id;
    progress.progress = p;
    emit(progress);
  }
  WorkerEvent produced;
  produced.type = WorkerEventType::kArtifactProduced;
  produced.task_id = request.task_id;
  produced.artifact_ref = MockArtifactHash(request);
  emit(produced);
  WorkerEvent completed;
  completed.type = WorkerEventType::kCompleted;
  completed.task_id = request.task_id;
  emit(completed);
}

InProcessExecutor::InProcessExecutor(ResourceProfile profile,
                                     InProcessTaskRunner runner)
    : profile_(std::move(profile)), id_(GenerateUuid()) {
  runner_ = runner ? std::move(runner)
                   : static_cast<InProcessTaskRunner>(DefaultInProcessRunner);
}

const ResourceProfile& InProcessExecutor::profile() const { return profile_; }

Uuid InProcessExecutor::id() const { return id_; }

std::string InProcessExecutor::implementation_label() const {
  return "inprocess";
}

InProcessExecutor::~InProcessExecutor() {
  if (worker_thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelled_ = true;
    }
    worker_thread_.join();
  }
}

void InProcessExecutor::Submit(const TaskRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) {
    throw WorkerError(ErrorCode::kWorkerTerminated,
                      "in-process worker is shut down");
  }
  if (busy_) {
    throw WorkerError(ErrorCode::kWorkerBusy,
                      "in-process worker is busy (max_concurrency = 1)");
  }
  if (worker_thread_.joinable()) {
    worker_thread_.join();  // the previous task has completed
  }
  busy_ = true;
  running_task_ = request.task_id;
  cancelled_ = false;
  cancel_reason_.clear();
  worker_thread_ =
      std::thread([this, request]() mutable { RunTask(std::move(request)); });
}

void InProcessExecutor::RunTask(TaskRequest request) {
  auto emit = [this](WorkerEvent event) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      events_.push_back(std::move(event));
    }
    cv_.notify_all();
  };
  auto cancelled = [this]() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
  };
  try {
    runner_(request, emit, cancelled);
  } catch (const std::exception& ex) {
    WorkerEvent failed;
    failed.type = WorkerEventType::kFailed;
    failed.task_id = request.task_id;
    failed.error_code = "WORKER_CRASHED";
    failed.error_message = std::string("in-process runner threw: ") + ex.what();
    failed.recoverable = true;
    emit(failed);
  } catch (...) {
    WorkerEvent failed;
    failed.type = WorkerEventType::kFailed;
    failed.task_id = request.task_id;
    failed.error_code = "WORKER_CRASHED";
    failed.error_message = "in-process runner threw unknown exception";
    failed.recoverable = true;
    emit(failed);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_ = false;
    running_task_ = {};
  }
  cv_.notify_all();
}

void InProcessExecutor::Cancel(const Uuid& task_id, const std::string& reason) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (busy_ && running_task_ == task_id) {
    cancelled_ = true;
    cancel_reason_ = reason;
  }
}

bool InProcessExecutor::WaitForEvent(WorkerEvent& out, std::int64_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (events_.empty()) {
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this] { return !events_.empty() || shutdown_; });
  }
  if (events_.empty()) {
    return false;
  }
  out = std::move(events_.front());
  events_.pop_front();
  return true;
}

void InProcessExecutor::Shutdown() {
  std::thread to_join;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
    cancelled_ = true;
    to_join = std::move(worker_thread_);
  }
  cv_.notify_all();
  if (to_join.joinable()) {
    to_join.join();
  }
}

}  // namespace spatial::engine
