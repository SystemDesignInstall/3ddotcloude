// POSIX implementation of ChildProcess (ADR-011). Spawns via fork/exec with
// pipe() stdio; timed reads use poll() so no read blocks indefinitely. stderr
// is captured on a separate pipe.

#include "engine/workers/child_process.h"

#if defined(_WIN32)
#error child_process_posix.cpp must only be built on POSIX
#endif

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
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
};

namespace {

// Reads from `fd` with a deadline. Returns 0 = data, 1 = timeout, 2 = EOF/error.
int ReadWithDeadline(int fd, void* data, std::size_t size,
                     std::size_t& nread, std::int64_t timeout_ms) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  const int r = poll(&pfd, 1, static_cast<int>(timeout_ms));
  if (r < 0) {
    nread = 0;
    return 2;
  }
  if (r == 0) {
    nread = 0;
    return 1;
  }
  if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
    nread = 0;
    return 2;
  }
  const ssize_t n = read(fd, data, size);
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

  int stdin_pipe[2];
  int stdout_pipe[2];
  int stderr_pipe[2];
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0 ||
      pipe(stderr_pipe) != 0) {
    error = "spawn: pipe() failed: " + std::string(std::strerror(errno));
    return nullptr;
  }

  pid_t pid = fork();
  if (pid < 0) {
    error = "spawn: fork() failed: " + std::string(std::strerror(errno));
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    return nullptr;
  }

  if (pid == 0) {
    // Child: rewire stdio, close the parent ends, exec.
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);

    std::vector<char*> exec_argv;
    exec_argv.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
      exec_argv.push_back(const_cast<char*>(arg.c_str()));
    }
    exec_argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    if (!env.empty()) {
      // Merge overrides into the parent environment (the child must keep PATH
      // and any existing PYTHONPATH); overrides win on duplicate names.
      for (char** cur = environ; cur != nullptr && *cur != nullptr; ++cur) {
        env_storage.push_back(*cur);
      }
      for (const auto& kv : env) {
        const std::string name = kv.substr(0, kv.find('='));
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
    }
    if (env.empty()) {
      execv(exec_argv[0], exec_argv.data());
    } else {
      std::vector<char*> exec_envp;
      exec_envp.reserve(env_storage.size() + 1);
      for (auto& kv : env_storage) {
        exec_envp.push_back(const_cast<char*>(kv.c_str()));
      }
      exec_envp.push_back(nullptr);
      execve(exec_argv[0], exec_argv.data(), exec_envp.data());
    }
    // exec failed: report on stderr and exit nonzero.
    const char* msg = "spawn: exec failed\n";
    const ssize_t unused = write(STDERR_FILENO, msg, std::strlen(msg));
    (void)unused;
    _exit(127);
  }

  // Parent: close the child ends.
  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);

  auto impl = std::make_unique<Impl>();
  impl->pid_ = pid;
  impl->stdin_write_ = stdin_pipe[1];
  impl->stdout_read_ = stdout_pipe[0];
  impl->stderr_read_ = stderr_pipe[0];
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
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < size) {
    const ssize_t n =
        write(impl_->stdin_write_, bytes + written, size - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
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
  const pid_t r = waitpid(impl_->pid_, &status, WNOHANG);
  if (r == impl_->pid_) {
    impl_->reaped_ = true;
    impl_->exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return false;
  }
  return true;
}

void ChildProcess::Terminate() {
  if (impl_ == nullptr || impl_->pid_ < 0) {
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
    if (waitpid(impl_->pid_, &status, 0) == impl_->pid_) {
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
