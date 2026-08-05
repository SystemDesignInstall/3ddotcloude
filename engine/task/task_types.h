#pragma once

// Task model primitives (task-model.md, RFC-0003 §5.2/§5.4). The six
// lowercase states and the retry/cancellation/cache policies are the single
// source of truth shared by the scheduler, the workers, and the worker
// protocol (ADR-020).

#include <cstdint>
#include <string>
#include <string_view>

namespace spatial::engine {

// CAS content hash (SHA-256 hex, ADR-010). Tasks exchange data only as
// content-addressed refs, never as file paths (RFC-0003 §5.9).
using ArtifactRef = std::string;

// Six-state lifecycle (ADR-020). There is no created/ready/queued/retrying
// state: a task not yet dispatched is `pending`; retry is an attempt counter
// on a `failed` task that returns to `pending`.
enum class TaskStatus : int {
  kPending = 0,
  kRunning = 1,
  kSucceeded = 2,
  kFailed = 3,
  kCancelled = 4,
  kSkipped = 5,
};

// Canonical lowercase string (JSON, worker protocol, schema).
const char* TaskStatusName(TaskStatus status) noexcept;
TaskStatus ParseTaskStatus(std::string_view name);

// Exactly one terminal state per task is enforced as a property invariant
// (scheduler-design §3).
bool IsTerminal(TaskStatus status) noexcept;

enum class CancellationPolicy : int {
  kCooperative = 0,  // stop at the next safe checkpoint
  kBestEffort = 1,   // may finish a short critical section first
};

enum class CachePolicy : int {
  kCacheable = 0,  // consult the cache when the task is deterministic
  kNever = 1,      // bypass the cache entirely
};

// Failure propagation when a dependency fails (task-model §2): default
// `skipped` (producer ignored, consumer not attempted), or `failed` for a
// strict path.
enum class FailurePolicy : int {
  kSkipped = 0,  // dependent becomes skipped
  kFailed = 1,   // dependent becomes failed
};

struct RetryPolicy {
  int max_attempts = 2;          // total attempts before declared failed
  std::int64_t base_ns = 1000000000;      // 1 s
  double multiplier = 2.0;                // exponential backoff
  std::int64_t max_ns = 60000000000;      // 60 s cap

  // Bounded exponential backoff for `attempt` (1-based): delay before the
  // next retry after this attempt.
  std::int64_t BackoffNs(int attempt) const noexcept;
};

struct TaskMetadata {
  RetryPolicy retry;
  CancellationPolicy cancellation = CancellationPolicy::kCooperative;
  CachePolicy cache = CachePolicy::kCacheable;
  FailurePolicy failure = FailurePolicy::kSkipped;
  bool deterministic = false;  // identical inputs -> byte-identical outputs
  int attempts = 0;
  std::int64_t created_at_ns = 0;
  std::int64_t updated_at_ns = 0;
};

}  // namespace spatial::engine
