// POSIX implementation of the generic adapter-layer process runner (C1-S3;
// plan §9). fork/exec with anonymous pipes for stdout/stderr; two reader
// threads drain the pipes to completion. The deadline and cancel token are
// honored by polling waitpid(WNOHANG); on timeout or cancellation the child
// is SIGKILLed and reaped before returning.

#include "adapters/process/process_runner.h"

#ifdef _WIN32
#error process_runner_posix.cpp must only be built on POSIX
#endif

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace spatial::adapters::process {
namespace {

int MakePipePair(int fds[2]) {
  if (pipe(fds) != 0) {
    return -1;
  }
  // Non-blocking reads: the drain threads poll instead of blocking so a
  // killed child (whose write ends die with it) never stalls a join.
  const int flags = fcntl(fds[0], F_GETFL, 0);
  fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
  return 0;
}

void DrainPipe(int fd, std::string& out) {
  char buffer[65536];
  for (;;) {
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    if (n > 0) {
      out.append(buffer, static_cast<std::size_t>(n));
      continue;
    }
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

  int stdout_fds[2];
  int stderr_fds[2];
  if (MakePipePair(stdout_fds) != 0 || MakePipePair(stderr_fds) != 0) {
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

  // POSIX children inherit the parent environment; overrides are applied
  // inside the child with setenv before exec.
  const pid_t pid = fork();
  if (pid < 0) {
    result.error_message = std::string("run: fork failed: ") + strerror(errno);
    return result;
  }
  if (pid == 0) {
    // Child.
    close(stdout_fds[0]);
    close(stderr_fds[0]);
    dup2(stdout_fds[1], STDOUT_FILENO);
    dup2(stderr_fds[1], STDERR_FILENO);
    close(stdout_fds[1]);
    close(stderr_fds[1]);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
      _exit(126);  // cannot change directory
    }
    for (const auto& kv : spec.env) {
      const std::size_t eq = kv.find('=');
      if (eq != std::string::npos) {
        setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
      }
    }
    execvp(cargv[0], cargv.data());
    _exit(127);  // executable not found / not executable
  }

  // Parent: close the child's write ends; drain reads on threads.
  close(stdout_fds[1]);
  close(stderr_fds[1]);
  std::string stdout_text;
  std::string stderr_text;
  std::thread stdout_thread(DrainPipe, stdout_fds[0], std::ref(stdout_text));
  std::thread stderr_thread(DrainPipe, stderr_fds[0], std::ref(stderr_text));

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  bool cancelled = false;
  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      break;
    }
    if (waited < 0 && errno != EINTR) {
      break;  // waitpid error; treat the child as gone
    }
    if (cancel != nullptr && cancel->cancelled()) {
      cancelled = true;
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
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
        waitpid(pid, &status, 0);
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  stdout_thread.join();
  stderr_thread.join();
  close(stdout_fds[0]);
  close(stderr_fds[0]);

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
  if (WIFEXITED(status)) {
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
