// POSIX implementation of the generic adapter-layer process runner (C1-S3;
// plan §9). posix_spawnp with anonymous pipes for stdout/stderr; two reader
// threads drain the pipes to completion. The deadline and cancel token are
// honored by polling waitpid(WNOHANG); on timeout or cancellation the child
// is SIGKILLed and reaped before returning.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "adapters/process/process_runner.h"

#ifdef _WIN32
#error process_runner_posix.cpp must only be built on POSIX
#endif

#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

extern char** environ;

namespace spatial::adapters::process {
namespace {

struct PipePair {
  int ends[2]{-1, -1};

  ~PipePair() {
    for (const int fd : ends) if (fd >= 0) close(fd);
  }

  bool Open() {
#if defined(__linux__)
    if (pipe2(ends, O_CLOEXEC) != 0) return false;
#else
    if (pipe(ends) != 0) return false;
    for (const int fd : ends) {
      if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
    }
#endif
    for (int& fd : ends) {
      if (fd > STDERR_FILENO) continue;
      const int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      if (moved < 0) return false;
      close(fd);
      fd = moved;
    }
    const int flags = fcntl(ends[0], F_GETFL, 0);
    return flags >= 0 && fcntl(ends[0], F_SETFL, flags | O_NONBLOCK) >= 0;
  }

  void CloseWrite() {
    close(ends[1]);
    ends[1] = -1;
  }
};

void DrainPipe(int fd, std::string& out) {
  char buffer[65536];
  for (;;) {
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      out.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    return;  // EOF or error
  }
}

}  // namespace

ProcessResult RunSubprocess(const ProcessSpec& spec, std::int64_t timeout_ms,
                            const CancelToken* cancel) {
  ProcessResult result;
  if (spec.argv.empty()) {
    result.error_message = "run: empty argv";
    return result;
  }

  PipePair stdout_pipe;
  PipePair stderr_pipe;
  if (!stdout_pipe.Open() || !stderr_pipe.Open()) {
    result.error_message = std::string("run: pipe failed: ") + strerror(errno);
    return result;
  }

  const std::string cwd = spec.working_directory.empty()
                              ? std::string()
                              : spec.working_directory.string();
  std::vector<char*> cargv;
  cargv.reserve(spec.argv.size() + 1);
  for (const auto& arg : spec.argv) {
    cargv.push_back(const_cast<char*>(arg.c_str()));
  }
  cargv.push_back(nullptr);

  // Prepare the merged environment before spawning: no allocation or setenv
  // may run in a forked child of a multi-threaded worker.
  std::vector<std::string> env_storage;
  for (char** cur = environ; cur != nullptr && *cur != nullptr; ++cur) {
    env_storage.emplace_back(*cur);
  }
  for (const auto& kv : spec.env) {
    const auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string name = kv.substr(0, eq);
    for (auto it = env_storage.begin(); it != env_storage.end();) {
      if (it->compare(0, name.size(), name) == 0 && it->size() > name.size() &&
          (*it)[name.size()] == '=') {
        it = env_storage.erase(it);
      } else {
        ++it;
      }
    }
    env_storage.push_back(kv);
  }
  std::vector<char*> envp;
  for (auto& kv : env_storage) envp.push_back(kv.data());
  envp.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  int code = posix_spawn_file_actions_init(&actions);
  if (code != 0) {
    result.error_message = "run: file actions failed: " +
                           std::string(std::strerror(code));
    return result;
  }
  const auto dup = [&](int from, int to) {
    if (code == 0) code = posix_spawn_file_actions_adddup2(&actions, from, to);
  };
  dup(stdout_pipe.ends[1], STDOUT_FILENO);
  dup(stderr_pipe.ends[1], STDERR_FILENO);
  for (const auto* pair : {&stdout_pipe, &stderr_pipe}) {
    for (const int fd : pair->ends) {
      if (code == 0) code = posix_spawn_file_actions_addclose(&actions, fd);
    }
  }
  if (code == 0 && !cwd.empty()) {
    code = posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
  }
  pid_t pid = -1;
  if (code == 0) {
    code = posix_spawnp(&pid, cargv[0], &actions, nullptr,
                        cargv.data(), envp.data());
  }
  posix_spawn_file_actions_destroy(&actions);
  if (code != 0) {
    result.error_message = "run: spawn failed: " +
                           std::string(std::strerror(code));
    return result;
  }

  // Parent: close the child's write ends; drain reads on threads.
  stdout_pipe.CloseWrite();
  stderr_pipe.CloseWrite();
  std::string stdout_text;
  std::string stderr_text;
  std::thread stdout_thread(DrainPipe, stdout_pipe.ends[0], std::ref(stdout_text));
  std::thread stderr_thread(DrainPipe, stderr_pipe.ends[0], std::ref(stderr_text));

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  bool cancelled = false;
  bool wait_failed = false;
  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      break;
    }
    if (waited < 0 && errno != EINTR) {
      wait_failed = true;
      result.error_message = "run: waitpid failed: " +
                             std::string(std::strerror(errno));
      break;
    }
    if (cancel != nullptr && cancel->cancelled()) {
      cancelled = true;
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      break;
    }
    if (timeout_ms >= 0) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed >= timeout_ms) {
        timed_out = true;
        kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  stdout_thread.join();
  stderr_thread.join();

  result.stdout_text = std::move(stdout_text);
  result.stderr_text = std::move(stderr_text);
  if (timed_out || cancelled) {
    result.outcome = timed_out ? ProcessOutcome::kTimedOut
                               : ProcessOutcome::kCancelled;
    result.exit_code = -1;
    result.error_message = timed_out ? "child process exceeded the time limit"
                                     : "child process was cancelled";
    return result;
  }
  if (wait_failed) {
    result.outcome = ProcessOutcome::kCompleted;
    result.exit_code = -1;
  } else if (WIFEXITED(status)) {
    result.outcome = ProcessOutcome::kCompleted;
    result.exit_code = WEXITSTATUS(status);
  } else {
    // Killed by a signal (or waitpid failed): deterministic signal death is
    // a completion only when the child chose to exit; otherwise report it as
    // a completion with a negative exit code.
    result.outcome = ProcessOutcome::kCompleted;
    result.exit_code = WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
  }
  return result;
}

}  // namespace spatial::adapters::process
