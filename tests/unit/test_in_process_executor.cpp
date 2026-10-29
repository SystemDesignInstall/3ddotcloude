#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/errors/project_error.h"
#include "core/utils/uuid.h"
#include "engine/workers/in_process_executor.h"
#include "engine/workers/worker_handle.h"
#include "tests/unit/engine_test_helpers.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::GenerateUuid;
using spatial::core::WorkerError;

TaskRequest MakeRequest() {
  TaskRequest req;
  req.task_id = GenerateUuid();
  req.task_type = "feature_extract";
  req.input_refs = {"h0"};
  req.config_json = R"({"threshold":0.5})";
  req.workspace = "temp/job/task";
  return req;
}

std::vector<WorkerEvent> Drain(InProcessExecutor& executor, std::size_t max) {
  std::vector<WorkerEvent> events;
  WorkerEvent event;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (events.size() < max && std::chrono::steady_clock::now() < deadline) {
    if (executor.WaitForEvent(event, 200)) {
      events.push_back(event);
    }
  }
  return events;
}

TEST(InProcessExecutor, DefaultRunnerProducesProgressArtifactCompleted) {
  InProcessExecutor executor(test::BigWorker());
  executor.Submit(MakeRequest());

  const auto events = Drain(executor, 10);
  ASSERT_GE(events.size(), 3u);
  EXPECT_EQ(events.front().type, WorkerEventType::kProgress);
  EXPECT_EQ(events.back().type, WorkerEventType::kCompleted);

  bool saw_artifact = false;
  bool saw_completed = false;
  for (const auto& e : events) {
    if (e.type == WorkerEventType::kArtifactProduced) {
      saw_artifact = true;
      // Deterministic content hash over task identity + inputs (ADR-020).
      EXPECT_EQ(e.artifact_ref.size(), 64u);
    }
    if (e.type == WorkerEventType::kCompleted) {
      saw_completed = true;
    }
  }
  EXPECT_TRUE(saw_artifact);
  EXPECT_TRUE(saw_completed);
}

TEST(InProcessExecutor, CustomRunnerCanFailRecoverable) {
  InProcessExecutor executor(test::BigWorker(), [](const TaskRequest& req,
                                                   auto emit, auto) {
    WorkerEvent failed;
    failed.type = WorkerEventType::kFailed;
    failed.task_id = req.task_id;
    failed.error_code = "ADAPTER_ERROR";
    failed.error_message = "feature extraction failed";
    failed.recoverable = true;
    emit(failed);
  });
  executor.Submit(MakeRequest());

  const auto events = Drain(executor, 2);
  ASSERT_GE(events.size(), 1u);
  EXPECT_EQ(events[0].type, WorkerEventType::kFailed);
  EXPECT_EQ(events[0].error_code, "ADAPTER_ERROR");
  EXPECT_TRUE(events[0].recoverable);
}

TEST(InProcessExecutor, PreservesPermanentDomainFailure) {
  const spatial::core::ValidationError error(
      ErrorCode::kValidationDomain, "bundle adjustment quality gate rejected");
  InProcessExecutor executor(test::BigWorker(),
                            [error](const TaskRequest&, auto, auto) {
                              throw error;
                            });
  const auto request = MakeRequest();
  executor.Submit(request);

  const auto events = Drain(executor, 1);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, WorkerEventType::kFailed);
  EXPECT_EQ(events[0].task_id, request.task_id);
  EXPECT_EQ(events[0].error_code, "VALIDATION_DOMAIN");
  EXPECT_EQ(events[0].error_message, error.what());
  EXPECT_FALSE(events[0].recoverable);
}

TEST(InProcessExecutor, PreservesTransientDomainFailure) {
  const spatial::core::StorageError error(
      ErrorCode::kStorageLock, "storage temporarily locked", {}, true);
  InProcessExecutor executor(test::BigWorker(),
                            [error](const TaskRequest&, auto, auto) {
                              throw error;
                            });
  executor.Submit(MakeRequest());

  const auto events = Drain(executor, 1);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, WorkerEventType::kFailed);
  EXPECT_EQ(events[0].error_code, "STORAGE_LOCK");
  EXPECT_EQ(events[0].error_message, error.what());
  EXPECT_TRUE(events[0].recoverable);
}

TEST(InProcessExecutor, UnknownStandardExceptionRemainsRecoverableCrash) {
  InProcessExecutor executor(test::BigWorker(),
                            [](const TaskRequest&, auto, auto) {
                              throw std::runtime_error("unclassified failure");
                            });
  executor.Submit(MakeRequest());

  const auto events = Drain(executor, 1);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, WorkerEventType::kFailed);
  EXPECT_EQ(events[0].error_code, "WORKER_CRASHED");
  EXPECT_NE(events[0].error_message.find("unclassified failure"),
            std::string::npos);
  EXPECT_TRUE(events[0].recoverable);
}

TEST(InProcessExecutor, UnknownExceptionRemainsRecoverableCrash) {
  InProcessExecutor executor(test::BigWorker(),
                            [](const TaskRequest&, auto, auto) { throw 17; });
  executor.Submit(MakeRequest());

  const auto events = Drain(executor, 1);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, WorkerEventType::kFailed);
  EXPECT_EQ(events[0].error_code, "WORKER_CRASHED");
  EXPECT_TRUE(events[0].recoverable);
}

TEST(InProcessExecutor, TerminalEventWaitsForRunnerCleanupAndAllowsNextSubmit) {
  std::mutex cleanup_mutex;
  std::condition_variable cleanup_cv;
  bool cleanup_started = false;
  bool release_cleanup = false;
  InProcessExecutor executor(
      test::BigWorker(), [&](const TaskRequest& request, auto emit, auto) {
        WorkerEvent completed;
        completed.type = WorkerEventType::kCompleted;
        completed.task_id = request.task_id;
        emit(completed);
        std::unique_lock<std::mutex> lock(cleanup_mutex);
        cleanup_started = true;
        cleanup_cv.notify_all();
        cleanup_cv.wait(lock, [&] { return release_cleanup; });
      });
  executor.Submit(MakeRequest());

  WorkerEvent event;
  bool reached_cleanup = false;
  bool terminal_before_return = false;
  {
    std::unique_lock<std::mutex> lock(cleanup_mutex);
    reached_cleanup = cleanup_cv.wait_for(
        lock, std::chrono::seconds(5), [&] { return cleanup_started; });
    terminal_before_return = executor.WaitForEvent(event, 0);
    // Always release the runner before any fatal test assertion.
    release_cleanup = true;
  }
  cleanup_cv.notify_all();

  EXPECT_TRUE(reached_cleanup);
  EXPECT_FALSE(terminal_before_return);
  if (!terminal_before_return) {
    ASSERT_TRUE(executor.WaitForEvent(event, 2000));
  }
  EXPECT_EQ(event.type, WorkerEventType::kCompleted);
  const auto next = MakeRequest();
  ASSERT_NO_THROW(executor.Submit(next));
  ASSERT_TRUE(executor.WaitForEvent(event, 2000));
  EXPECT_EQ(event.task_id, next.task_id);
  EXPECT_EQ(event.type, WorkerEventType::kCompleted);
}

TEST(InProcessExecutor, RejectsSecondTaskWhileBusy) {
  InProcessExecutor executor(
      test::BigWorker(),
      [](const TaskRequest& req, auto emit, auto cancelled) {
        while (!cancelled()) {
          // Run until cancelled; keeps busy_ = true for the assertion below.
        }
        WorkerEvent cancelled_event;
        cancelled_event.type = WorkerEventType::kCancelled;
        cancelled_event.task_id = req.task_id;
        emit(cancelled_event);
      });
  executor.Submit(MakeRequest());
  EXPECT_THROW(
      {
        try {
          executor.Submit(MakeRequest());
        } catch (const WorkerError& ex) {
          EXPECT_EQ(ex.code(), ErrorCode::kWorkerBusy);
          throw;
        }
      },
      WorkerError);
  executor.Cancel(MakeRequest().task_id, "");  // no-op: wrong id, no crash
  executor.Shutdown();
}

TEST(InProcessExecutor, CooperativeCancellationIsDelivered) {
  InProcessExecutor executor(
      test::BigWorker(),
      [](const TaskRequest& req, auto emit, auto cancelled) {
        emit([&] {
          WorkerEvent p;
          p.type = WorkerEventType::kProgress;
          p.task_id = req.task_id;
          p.progress = 10;
          return p;
        }());
        while (!cancelled()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        WorkerEvent cancelled_event;
        cancelled_event.type = WorkerEventType::kCancelled;
        cancelled_event.task_id = req.task_id;
        emit(cancelled_event);
      });

  const auto req = MakeRequest();
  executor.Submit(req);
  const auto first = Drain(executor, 1);
  ASSERT_GE(first.size(), 1u);
  EXPECT_EQ(first[0].type, WorkerEventType::kProgress);

  executor.Cancel(req.task_id, "user aborted");
  const auto rest = Drain(executor, 2);
  ASSERT_GE(rest.size(), 1u);
  EXPECT_EQ(rest[0].type, WorkerEventType::kCancelled);
  executor.Shutdown();
}

TEST(InProcessExecutor, ShutdownJoinsRunningTask) {
  InProcessExecutor executor(
      test::BigWorker(),
      [](const TaskRequest&, auto, auto cancelled) {
        while (!cancelled()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      });
  executor.Submit(MakeRequest());
  executor.Shutdown();  // must not deadlock; sets the cancel flag
}

TEST(InProcessExecutor, ThrowsAfterShutdown) {
  InProcessExecutor executor(test::BigWorker());
  executor.Shutdown();
  EXPECT_THROW(
      {
        try {
          executor.Submit(MakeRequest());
        } catch (const WorkerError& ex) {
          EXPECT_EQ(ex.code(), ErrorCode::kWorkerTerminated);
          throw;
        }
      },
      WorkerError);
}

}  // namespace
}  // namespace spatial::engine
