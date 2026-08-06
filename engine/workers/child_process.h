#pragma once

// Cross-platform child-process primitive for worker supervision (ADR-011,
// worker-protocol §1). Wraps CreateProcess (Win32) and fork/exec (POSIX)
// with anonymous stdin/stdout/stderr pipes, blocking writes and timed reads.
//
// The engine owns the full worker lifecycle: spawn, feed frames, read frames
// with a deadline, detect exit/EOF (crash), terminate, and reap. Reads never
// block indefinitely: every Read* call takes a timeout so the scheduler can
// keep its heartbeat supervision loop.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spatial::engine {

class ChildProcess {
 public:
  // Spawns `argv[0]` (with arguments) with its stdio connected to anonymous
  // pipes. `env` is an optional list of "KEY=VALUE" overrides (when empty the
  // child inherits the parent environment). Returns a process, or nullptr
  // with a message in `error` on spawn failure. The child stderr is captured
  // through a separate pipe (worker stderr is diagnostics only,
  // worker-protocol §1).
  static std::unique_ptr<ChildProcess> Spawn(
      const std::vector<std::string>& argv, std::string& error,
      const std::vector<std::string>& env = {});

  ~ChildProcess();

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // Writes the full buffer to the child's stdin. Returns false on a broken
  // pipe (the child exited) with `error` set.
  bool Write(const void* data, std::size_t size, std::string& error);

  // Reads up to `size` bytes from the child's stdout, waiting at most
  // `timeout_ms`. Returns true and sets `nread` on data, or returns false:
  //  * eof = true  -> stdout closed / child exited without more data;
  //  * eof = false -> timed out with no data available.
  // On an I/O error `error` is set.
  bool ReadStdout(void* data, std::size_t size, std::int64_t timeout_ms,
                  std::size_t& nread, bool& eof, std::string& error);

  // Same for stderr (diagnostics channel).
  bool ReadStderr(void* data, std::size_t size, std::int64_t timeout_ms,
                  std::size_t& nread, bool& eof, std::string& error);

  // True while the process has not been reaped (running or exited-but-unreaped).
  bool IsRunning();

  // Forcefully terminates the child (TerminateProcess / SIGKILL). No-op if
  // already exited. Does not reap.
  void Terminate();

  // Reaps the child and returns its exit code (0 on clean exit). Returns -1
  // if the process was killed by a signal or the exit code cannot be read.
  int Wait();

 private:
  struct Impl;
  explicit ChildProcess(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace spatial::engine
