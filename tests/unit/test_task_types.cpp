#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "core/errors/project_error.h"
#include "engine/task/task_types.h"

namespace spatial::engine {
namespace {

using spatial::core::ValidationError;

TEST(TaskTypes, StatusNamesRoundTrip) {
  for (int i = 0; i < 6; ++i) {
    const auto status = static_cast<TaskStatus>(i);
    EXPECT_EQ(ParseTaskStatus(TaskStatusName(status)), status);
  }
  EXPECT_STREQ(TaskStatusName(TaskStatus::kPending), "pending");
  EXPECT_STREQ(TaskStatusName(TaskStatus::kRunning), "running");
  EXPECT_STREQ(TaskStatusName(TaskStatus::kSucceeded), "succeeded");
  EXPECT_STREQ(TaskStatusName(TaskStatus::kFailed), "failed");
  EXPECT_STREQ(TaskStatusName(TaskStatus::kCancelled), "cancelled");
  EXPECT_STREQ(TaskStatusName(TaskStatus::kSkipped), "skipped");
  EXPECT_THROW(ParseTaskStatus("queued"), ValidationError);
  EXPECT_THROW(ParseTaskStatus("retrying"), ValidationError);
}

TEST(TaskTypes, ExactlyOneTerminalState) {
  EXPECT_FALSE(IsTerminal(TaskStatus::kPending));
  EXPECT_FALSE(IsTerminal(TaskStatus::kRunning));
  EXPECT_TRUE(IsTerminal(TaskStatus::kSucceeded));
  EXPECT_TRUE(IsTerminal(TaskStatus::kFailed));
  EXPECT_TRUE(IsTerminal(TaskStatus::kCancelled));
  EXPECT_TRUE(IsTerminal(TaskStatus::kSkipped));
}

TEST(TaskTypes, BoundedExponentialBackoff) {
  RetryPolicy policy;
  EXPECT_EQ(policy.max_attempts, 2);
  EXPECT_EQ(policy.BackoffNs(1), std::int64_t{1000000000});
  EXPECT_EQ(policy.BackoffNs(2), std::int64_t{2000000000});
  EXPECT_EQ(policy.BackoffNs(3), std::int64_t{4000000000});
  policy.max_ns = 3000000000;
  EXPECT_EQ(policy.BackoffNs(3), std::int64_t{3000000000});
}

}  // namespace
}  // namespace spatial::engine
