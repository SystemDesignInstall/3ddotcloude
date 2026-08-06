#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
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
