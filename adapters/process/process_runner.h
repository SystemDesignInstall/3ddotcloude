#pragma once

// Generic subprocess runner for the adapter layer (C1-S3; plan §9
// "ProcessRunner" terminology). A pure process-execution primitive: spawn a
// child process with argv + working directory, capture its stdout and stderr
// in full (unbounded), wait with a deadline, and terminate + reap it on
// timeout or cooperative cancellation so no uncontrolled process is ever left
// behind.
//
// This is the "generic ProcessExecutor" of the adapter boundary: the engine's
// WorkerExecutor/ProcessExecutor is protocol-bound (framed protobuf over
// stdin/stdout) and lives in the engine, which adapters never link (RFC-0008
// §5). Adapters instead run their backends through this generic runner —
// COLMAP-specific argv construction stays in adapters/colmap/**, this type is
// backend-agnostic and has no COLMAP type or include.
//
// Threading: captures output on dedicated reader threads so a chatty child
// can never deadlock on a full pipe buffer. One run at a time; the worker
// owns its single in-flight task (plan §4 concurrency = 1).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace spatial::adapters::process {

// What to run. `argv[0]` is the executable (absolute path or resolvable on
// PATH); the remainder are arguments. `working_directory` is the child's
// current directory (empty = inherit the parent's). `env` is an optional list
// of "KEY=VALUE" overrides merged over the parent environment (empty =
// inherit unchanged).
struct ProcessSpec {
  std::vector<std::string> argv;
  std::filesystem::path working_directory;
  std::vector<std::string> env;
};

enum class ProcessOutcome : int {
  kCompleted = 0,     // child ran to completion; exit_code is valid
  kTimedOut = 1,      // the deadline expired; child was terminated and reaped
  kCancelled = 2,     // the cancel token fired; child was terminated and reaped
  kSpawnFailed = 3,   // the process could not be started; see error_message
};

struct ProcessResult {
  ProcessOutcome outcome = ProcessOutcome::kSpawnFailed;
  int exit_code = -1;        // valid only when outcome == kCompleted
  std::string stdout_text;   // captured in full
  std::string stderr_text;   // captured in full
  std::string error_message; // spawn / termination diagnostics (may be empty)
};

// Thread-safe cooperative-cancellation flag (worker-protocol TaskCancelled
// mapped by the host into this token).
class CancelToken {
 public:
  void Cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
  bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> cancelled_{false};
};

// Runs `spec.argv` to completion (or until the deadline / cancellation),
// capturing both output streams. On kTimedOut / kCancelled the child is
// forcefully terminated and reaped before returning — no orphan process, no
// stale pipe. `timeout_ms` < 0 waits indefinitely. `cancel` is optional.
// Not thread-safe for concurrent calls; one child at a time.
ProcessResult RunSubprocess(const ProcessSpec& spec, std::int64_t timeout_ms,
                            const CancelToken* cancel = nullptr);

}  // namespace spatial::adapters::process
