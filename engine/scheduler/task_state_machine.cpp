#include "engine/scheduler/task_state_machine.h"

#include <string>

#include "core/errors/project_error.h"

namespace spatial::engine {

using spatial::core::ErrorCode;
using spatial::core::SchedulerError;

bool TaskStateMachine::IsLegal(TaskStatus from, TaskStatus to) noexcept {
  // The retry edge is the single exception to the terminal-state rule
  // (failed -> pending), gated externally by the retry policy.
  if (from == TaskStatus::kFailed) {
    return to == TaskStatus::kPending;
  }
  if (IsTerminal(from)) {
    return false;
  }
  switch (from) {
    case TaskStatus::kPending:
      // `failed` is the strict-path failure propagation (task-model §2): a
      // dependent that never dispatched is marked failed to fail the job.
      return to == TaskStatus::kRunning || to == TaskStatus::kCancelled ||
             to == TaskStatus::kSkipped || to == TaskStatus::kFailed;
    case TaskStatus::kRunning:
      return to == TaskStatus::kSucceeded || to == TaskStatus::kFailed ||
             to == TaskStatus::kCancelled;
    default:
      return false;
  }
}

TaskStatus TaskStateMachine::Transition(TaskStatus from, TaskStatus to) {
  if (!IsLegal(from, to)) {
    throw SchedulerError(
        ErrorCode::kInternal,
        "illegal task state transition " + std::string(TaskStatusName(from)) +
            " -> " + TaskStatusName(to));
  }
  return to;
}

}  // namespace spatial::engine
