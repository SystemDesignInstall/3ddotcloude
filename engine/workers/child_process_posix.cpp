// POSIX implementation of ChildProcess (ADR-011). Spawns via posix_spawnp with
// pipe() stdio; timed reads use poll() so no read blocks indefinitely. stderr
// is captured on a separate pipe.

#include "engine/workers/child_process.h"

#if defined(_WIN32)
#error child_process_posix.cpp must only be built on POSIX
#endif

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

extern char** environ;

namespace spatial::engine {

struct ChildProcess::Impl {
  pid_t pid_ = -1;
  int stdin_write_ = -1;  // engine -> child stdin
  int stdout_read_ = -1;  // child stdout -> engine
  int stderr_read_ = -1;  // child stderr -> engine
  bool reaped_ = false;
  int exit_code_ = -1;

  ~Impl() {
    if (stdin_write_ >= 0) close(stdin_write_);
    if (stdout_read_ >= 0) close(stdout_read_);
    if (stderr_read_ >= 0) close(stderr_read_);
  }
};

namespace {

// A failed pipe write must report EPIPE rather than terminate the engine.
// Suppress SIGPIPE only on this thread and consume only the signal generated
// by this write; never change the process-wide signal disposition.
class ScopedSigpipeBlock {
 public:
  ScopedSigpipeBlock() {
    sigemptyset(&blocked_);
    sigaddset(&blocked_, SIGPIPE);
    code_ = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_);
    if (code_ != 0) return;
    sigset_t pending;
    if (sigpending(&pending) != 0) {
      code_ = errno;
      pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
      return;
    }
    active_ = true;
    consume_ = sigismember(&previous_, SIGPIPE) == 0 &&
               sigismember(&pending, SIGPIPE) == 0;
  }

  ~ScopedSigpipeBlock() {
    if (!active_) return;
    if (broken_pipe_ && consume_) {
      const timespec immediate{0, 0};
      while (sigtimedwait(&blocked_, nullptr, &immediate) < 0 && errno == EINTR) {}
    }
    pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
  }

  int code() const { return code_; }
  void BrokenPipe() { broken_pipe_ = true; }

 private:
  sigset_t blocked_{};
  sigset_t previous_{};
  int code_ = 0;
  bool active_ = false;
  bool consume_ = false;
  bool broken_pipe_ = false;
};

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
    // Do not let file actions accidentally close stdio when the parent had a
    // closed standard descriptor before creating these pipes.
    for (int& fd : ends) {
      if (fd > STDERR_FILENO) continue;
      const int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      if (moved < 0) return false;
      close(fd);
      fd = moved;
    }
    return true;
  }

  int Release(int index) { return std::exchange(ends[index], -1); }
};

// Reads from `fd` with a deadline. Returns 0 = data, 1 = timeout, 2 = EOF/error.
int ReadWithDeadline(int fd, void* data, std::size_t size,
                     std::size_t& nread, std::int64_t timeout_ms) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  int r;
  do {
    r = poll(&pfd, 1, static_cast<int>(timeout_ms));
  } while (r < 0 && errno == EINTR);
  if (r < 0) {
    nread = 0;
    return 2;
  }
  if (r == 0) {
    nread = 0;
    return 1;
  }
  if ((pfd.revents & POLLNVAL) != 0) {
    nread = 0;
    return 2;
  }
  // POLLIN and POLLHUP may arrive together. Drain buffered bytes before EOF,
  // including the final diagnostic or WorkerHello from a short-lived child.
  ssize_t n;
  do {
    n = read(fd, data, size);
  } while (n < 0 && errno == EINTR);
  if (n < 0) {
    nread = 0;
    return 2;
  }
  if (n == 0) {
    nread = 0;
    return 2;  // EOF
  }
  nread = static_cast<std::size_t>(n);
  return 0;
}

}  // namespace

std::unique_ptr<ChildProcess> ChildProcess::Spawn(
    const std::vector<std::string>& argv, std::string& error,
    const std::vector<std::string>& env) {
  if (argv.empty()) {
    error = "spawn: empty argv";
    return nullptr;
  }

  PipePair stdin_pipe;
  PipePair stdout_pipe;
  PipePair stderr_pipe;
  if (!stdin_pipe.Open() || !stdout_pipe.Open() || !stderr_pipe.Open()) {
    error = "spawn: pipe() failed: " + std::string(std::strerror(errno));
    return nullptr;
  }

  // Prepare allocations in the parent. posix_spawnp both searches PATH and
  // reports exec failure synchronously, without running C++/setenv after fork
  // in a potentially multi-threaded engine.
  std::vector<char*> exec_argv;
  for (const auto& arg : argv) {
    exec_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  exec_argv.push_back(nullptr);
  std::vector<std::string> env_storage;
  for (char** cur = environ; cur != nullptr && *cur != nullptr; ++cur) {
    env_storage.push_back(*cur);
  }
  for (const auto& kv : env) {
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
  std::vector<char*> exec_envp;
  for (auto& kv : env_storage) exec_envp.push_back(kv.data());
  exec_envp.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  int code = posix_spawn_file_actions_init(&actions);
  if (code != 0) {
    error = "spawn: file actions failed: " + std::string(std::strerror(code));
    return nullptr;
  }
  const auto dup = [&](int from, int to) {
    if (code == 0) code = posix_spawn_file_actions_adddup2(&actions, from, to);
  };
  dup(stdin_pipe.ends[0], STDIN_FILENO);
  dup(stdout_pipe.ends[1], STDOUT_FILENO);
  dup(stderr_pipe.ends[1], STDERR_FILENO);
  for (const auto* pair : {&stdin_pipe, &stdout_pipe, &stderr_pipe}) {
    for (const int fd : pair->ends) {
      if (code == 0) code = posix_spawn_file_actions_addclose(&actions, fd);
    }
  }
  pid_t pid = -1;
  if (code == 0) {
    code = posix_spawnp(&pid, exec_argv[0], &actions, nullptr,
                        exec_argv.data(), exec_envp.data());
  }
  posix_spawn_file_actions_destroy(&actions);
  if (code != 0) {
    error = "spawn: exec failed: " + std::string(std::strerror(code));
    return nullptr;
  }

  auto impl = std::make_unique<Impl>();
  impl->pid_ = pid;
  impl->stdin_write_ = stdin_pipe.Release(1);
  impl->stdout_read_ = stdout_pipe.Release(0);
  impl->stderr_read_ = stderr_pipe.Release(0);
  error.clear();
  return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ChildProcess::~ChildProcess() {
  if (impl_) {
    if (IsRunning()) {
      Terminate();
      Wait();
    }
  }
}

bool ChildProcess::Write(const void* data, std::size_t size, std::string& error) {
  if (impl_ == nullptr || impl_->stdin_write_ < 0) {
    error = "write: stdin pipe closed";
    return false;
  }
  ScopedSigpipeBlock sigpipe;
  if (sigpipe.code() != 0) {
    error = "write: cannot block SIGPIPE: " +
            std::string(std::strerror(sigpipe.code()));
    return false;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < size) {
    const ssize_t n =
        write(impl_->stdin_write_, bytes + written, size - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EPIPE) sigpipe.BrokenPipe();
      error = "write: broken stdin pipe: " + std::string(std::strerror(errno));
      return false;
    }
    if (n == 0) {
      error = "write: child closed stdin";
      return false;
    }
    written += static_cast<std::size_t>(n);
  }
  return true;
}

bool ChildProcess::ReadStdout(void* data, std::size_t size, std::int64_t timeout_ms,
                              std::size_t& nread, bool& eof, std::string& error) {
  if (impl_ == nullptr || impl_->stdout_read_ < 0) {
    nread = 0;
    eof = true;
    error.clear();
    return false;
  }
  nread = 0;
  const int r =
      ReadWithDeadline(impl_->stdout_read_, data, size, nread, timeout_ms);
  if (r == 0) {
    eof = false;
    return true;
  }
  eof = (r == 2);
  return false;
}

bool ChildProcess::ReadStderr(void* data, std::size_t size, std::int64_t timeout_ms,
                              std::size_t& nread, bool& eof, std::string& error) {
  if (impl_ == nullptr || impl_->stderr_read_ < 0) {
    nread = 0;
    eof = true;
    error.clear();
    return false;
  }
  nread = 0;
  const int r =
      ReadWithDeadline(impl_->stderr_read_, data, size, nread, timeout_ms);
  if (r == 0) {
    eof = false;
    return true;
  }
  eof = (r == 2);
  return false;
}

bool ChildProcess::IsRunning() {
  if (impl_ == nullptr || impl_->pid_ < 0 || impl_->reaped_) {
    return false;
  }
  int status = 0;
  pid_t r;
  do {
    r = waitpid(impl_->pid_, &status, WNOHANG);
  } while (r < 0 && errno == EINTR);
  if (r == impl_->pid_) {
    impl_->reaped_ = true;
    impl_->exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return false;
  }
  if (r < 0 && errno == ECHILD) {
    impl_->reaped_ = true;
    return false;
  }
  return true;
}

void ChildProcess::Terminate() {
  if (impl_ == nullptr || impl_->pid_ < 0 || impl_->reaped_) {
    return;
  }
  kill(impl_->pid_, SIGKILL);
}

int ChildProcess::Wait() {
  if (impl_ == nullptr || impl_->pid_ < 0) {
    return -1;
  }
  if (!impl_->reaped_) {
    int status = 0;
    pid_t waited;
    do {
      waited = waitpid(impl_->pid_, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited == impl_->pid_) {
      impl_->reaped_ = true;
      impl_->exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
  }
  if (impl_->stdin_write_ >= 0) {
    close(impl_->stdin_write_);
    impl_->stdin_write_ = -1;
  }
  return impl_->exit_code_;
}

}  // namespace spatial::engine
