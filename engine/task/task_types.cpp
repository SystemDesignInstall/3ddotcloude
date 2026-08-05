#include "engine/task/task_types.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/errors/project_error.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::ValidationError;

constexpr const char* kStatusNames[] = {"pending", "running", "succeeded",
                                        "failed",  "cancelled", "skipped"};

}  // namespace

const char* TaskStatusName(TaskStatus status) noexcept {
  const int i = static_cast<int>(status);
  return (i >= 0 && i < 6) ? kStatusNames[i] : "pending";
}

TaskStatus ParseTaskStatus(std::string_view name) {
  for (int i = 0; i < 6; ++i) {
    if (name == kStatusNames[i]) {
      return static_cast<TaskStatus>(i);
    }
  }
  throw ValidationError(ErrorCode::kValidationDomain,
                        "unknown task status: " + std::string(name));
}

bool IsTerminal(TaskStatus status) noexcept {
  switch (status) {
    case TaskStatus::kSucceeded:
    case TaskStatus::kFailed:
    case TaskStatus::kCancelled:
    case TaskStatus::kSkipped:
      return true;
    default:
      return false;
  }
}

std::int64_t RetryPolicy::BackoffNs(int attempt) const noexcept {
  const int exponent = std::max(0, attempt - 1);
  double delay = static_cast<double>(base_ns);
  for (int i = 0; i < exponent; ++i) {
    delay *= multiplier;
  }
  return static_cast<std::int64_t>(std::min(delay, static_cast<double>(max_ns)));
}

}  // namespace spatial::engine
