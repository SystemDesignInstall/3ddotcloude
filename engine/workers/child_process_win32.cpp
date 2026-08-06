// Windows implementation of ChildProcess (ADR-011). Spawns via CreateProcess
// with anonymous pipes; timed reads use PeekNamedPipe + a deadline loop so no
// read blocks indefinitely. stderr is captured on a separate pipe.

#include "engine/workers/child_process.h"

#ifndef _WIN32
#error child_process_win32.cpp must only be built on Windows
#endif

#define NOMINMAX
#include <windows.h>

#include <sstream>

#include <cwchar>
#include <map>

namespace spatial::engine {
namespace {

// One end of an anonymous pipe pair.
struct PipeEnds {
  HANDLE read_ = nullptr;   // inheritable by the child (child's end)
  HANDLE write_ = nullptr;  // inheritable by the child (child's end)
  ~PipeEnds();
};
PipeEnds::~PipeEnds() {
  if (read_) CloseHandle(read_);
  if (write_) CloseHandle(write_);
}

// Creates an anonymous pipe; returns false on failure.
bool MakePipe(PipeEnds& p, SECURITY_ATTRIBUTES& sa) {
  HANDLE r = nullptr, w = nullptr;
  if (!CreatePipe(&r, &w, &sa, 0)) {
    return false;
  }
  p.read_ = r;
  p.write_ = w;
  return true;
}

std::string WinErrorText(DWORD code);

// Builds a double-null-terminated Unicode environment block: the parent
// environment merged with `overrides`. Passing a custom block to
// CreateProcessW *replaces* the child environment, so dropping the parent
// entries would remove PATH and break the python interpreter (its DLLs are
// resolved via PATH). Overrides win on duplicate variable names.
std::wstring BuildEnvBlock(const std::vector<std::string>& overrides) {
  std::map<std::wstring, std::wstring> merged;
  LPWCH parent = GetEnvironmentStringsW();
  if (parent != nullptr) {
    for (LPWCH cur = parent; *cur != L'\0'; cur += wcslen(cur) + 1) {
      const std::wstring entry(cur);
      const std::size_t eq = entry.find(L'=');
      if (eq != std::wstring::npos) {
        merged[entry.substr(0, eq)] = entry.substr(eq + 1);
      }
    }
    FreeEnvironmentStringsW(parent);
  }
  for (const auto& kv : overrides) {
    const std::wstring wkv(kv.begin(), kv.end());
    const std::size_t eq = wkv.find(L'=');
    if (eq != std::wstring::npos) {
      merged[wkv.substr(0, eq)] = wkv.substr(eq + 1);
    }
  }
  std::wstring block;
  for (const auto& pair : merged) {
    block += pair.first;
    block += L'=';
    block += pair.second;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

// Clears the inheritance flag on a parent-owned handle.
bool DontInherit(HANDLE h, std::string& error) {
  if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0)) {
    error = "SetHandleInformation failed: " + WinErrorText(GetLastError());
    return false;
  }
  return true;
}

// Reads from `h` with a deadline. Returns 0 = data, 1 = timeout, 2 = EOF/error.
int ReadWithDeadline(HANDLE h, void* data, std::size_t size,
                     std::size_t& nread, std::int64_t timeout_ms) {
  const auto start = GetTickCount64();
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) {
      nread = 0;
      return 2;  // broken pipe / EOF
    }
    if (available > 0) {
      DWORD wanted = static_cast<DWORD>(available < size ? available : size);
      DWORD got = 0;
      if (!ReadFile(h, data, wanted, &got, nullptr)) {
        nread = 0;
        return 2;
      }
      nread = got;
      return 0;
    }
    if (GetTickCount64() - start >= static_cast<ULONGLONG>(timeout_ms)) {
      nread = 0;
      return 1;
    }
    Sleep(5);
  }
}

std::string WinErrorText(DWORD code) {
  LPWSTR buffer = nullptr;
  const DWORD len = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string text;
  if (len > 0 && buffer != nullptr) {
    const int need = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len),
                                         nullptr, 0, nullptr, nullptr);
    if (need > 0) {
      text.resize(need);
      WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len), text.data(),
                          need, nullptr, nullptr);
    }
    LocalFree(buffer);
  }
  if (text.empty()) {
    std::ostringstream os;
    os << "error " << code;
    text = os.str();
  }
  return text;
}

}  // namespace

struct ChildProcess::Impl {
  HANDLE process_ = nullptr;
  HANDLE stdin_write_ = nullptr;  // engine -> child stdin (parent-owned)
  PipeEnds stdout_pipe_;          // read_: parent-owned, write_: child's stdout
  PipeEnds stderr_pipe_;          // read_: parent-owned, write_: child's stderr
};

std::unique_ptr<ChildProcess> ChildProcess::Spawn(
    const std::vector<std::string>& argv, std::string& error,
    const std::vector<std::string>& env) {
  if (argv.empty()) {
    error = "spawn: empty argv";
    return nullptr;
  }
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  auto impl = std::make_unique<Impl>();

  PipeEnds stdin_pipe;  // read_: child's stdin, write_: parent writes to child
  if (!MakePipe(stdin_pipe, sa)) {
    error = "spawn: CreatePipe(stdin) failed: " + WinErrorText(GetLastError());
    return nullptr;
  }
  if (!DontInherit(stdin_pipe.write_, error)) {
    return nullptr;
  }
  impl->stdin_write_ = stdin_pipe.write_;

  if (!MakePipe(impl->stdout_pipe_, sa)) {
    error = "spawn: CreatePipe(stdout) failed: " + WinErrorText(GetLastError());
    return nullptr;
  }
  if (!DontInherit(impl->stdout_pipe_.read_, error)) {
    return nullptr;
  }

  if (!MakePipe(impl->stderr_pipe_, sa)) {
    error = "spawn: CreatePipe(stderr) failed: " + WinErrorText(GetLastError());
    return nullptr;
  }
  if (!DontInherit(impl->stderr_pipe_.read_, error)) {
    return nullptr;
  }

  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = stdin_pipe.read_;
  si.hStdOutput = impl->stdout_pipe_.write_;
  si.hStdError = impl->stderr_pipe_.write_;

  // Build the command line (CreateProcessW wants a single mutable string).
  std::wstring command_line;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      command_line += L' ';
    }
    const std::string& arg = argv[i];
    if (arg.find_first_of(" \t\"") != std::string::npos) {
      command_line += L'"';
      for (char c : arg) {
        if (c == '"') {
          command_line += L"\\\"";
        } else {
          command_line += static_cast<wchar_t>(c);
        }
      }
      command_line += L'"';
    } else {
      command_line.append(arg.begin(), arg.end());
    }
  }

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  std::wstring env_block;
  LPVOID env_ptr = nullptr;
  if (!env.empty()) {
    env_block = BuildEnvBlock(env);
    env_ptr = env_block.data();
  }
  const BOOL ok = CreateProcessW(
      nullptr, command_line.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
      /*dwCreationFlags=*/CREATE_UNICODE_ENVIRONMENT, env_ptr, nullptr, &si,
      &pi);
  if (!ok) {
    error = "spawn: CreateProcess failed: " + WinErrorText(GetLastError());
    return nullptr;
  }
  CloseHandle(pi.hThread);

  // The child inherits its own ends of the pipes; close them in the parent.
  // Ownership of stdin_pipe.write_ has moved to impl->stdin_write_, and the
  // explicit closes below clear the local handles so the PipeEnds destructor
  // does not double-close (which would kill the child's stdin pipe).
  CloseHandle(stdin_pipe.read_);
  CloseHandle(impl->stdout_pipe_.write_);
  CloseHandle(impl->stderr_pipe_.write_);
  stdin_pipe.read_ = nullptr;
  stdin_pipe.write_ = nullptr;
  impl->stdout_pipe_.write_ = nullptr;
  impl->stderr_pipe_.write_ = nullptr;

  impl->process_ = pi.hProcess;
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
  if (impl_ == nullptr || impl_->stdin_write_ == nullptr) {
    error = "write: stdin pipe closed";
    return false;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < size) {
    DWORD chunk = static_cast<DWORD>(size - written > (1u << 20) ? (1u << 20)
                                                                 : size - written);
    DWORD n = 0;
    if (!WriteFile(impl_->stdin_write_, bytes + written, chunk, &n, nullptr)) {
      error = "write: broken stdin pipe: " + WinErrorText(GetLastError());
      return false;
    }
    if (n == 0) {
      error = "write: child closed stdin";
      return false;
    }
    written += n;
  }
  return true;
}

bool ChildProcess::ReadStdout(void* data, std::size_t size, std::int64_t timeout_ms,
                              std::size_t& nread, bool& eof, std::string& error) {
  if (impl_ == nullptr || impl_->stdout_pipe_.read_ == nullptr) {
    nread = 0;
    eof = true;
    error.clear();
    return false;
  }
  nread = 0;
  const int r = ReadWithDeadline(impl_->stdout_pipe_.read_, data, size, nread,
                                 timeout_ms);
  if (r == 0) {
    eof = false;
    return true;
  }
  eof = (r == 2);
  return false;
}

bool ChildProcess::ReadStderr(void* data, std::size_t size, std::int64_t timeout_ms,
                              std::size_t& nread, bool& eof, std::string& error) {
  if (impl_ == nullptr || impl_->stderr_pipe_.read_ == nullptr) {
    nread = 0;
    eof = true;
    error.clear();
    return false;
  }
  nread = 0;
  const int r = ReadWithDeadline(impl_->stderr_pipe_.read_, data, size, nread,
                                 timeout_ms);
  if (r == 0) {
    eof = false;
    return true;
  }
  eof = (r == 2);
  return false;
}

bool ChildProcess::IsRunning() {
  if (impl_ == nullptr || impl_->process_ == nullptr) {
    return false;
  }
  const DWORD code = WaitForSingleObject(impl_->process_, 0);
  return code == WAIT_TIMEOUT;
}

void ChildProcess::Terminate() {
  if (impl_ == nullptr || impl_->process_ == nullptr) {
    return;
  }
  TerminateProcess(impl_->process_, 1u);
}

int ChildProcess::Wait() {
  if (impl_ == nullptr || impl_->process_ == nullptr) {
    return -1;
  }
  WaitForSingleObject(impl_->process_, INFINITE);
  DWORD code = 0;
  if (!GetExitCodeProcess(impl_->process_, &code)) {
    return -1;
  }
  CloseHandle(impl_->process_);
  impl_->process_ = nullptr;
  if (impl_->stdin_write_) {
    CloseHandle(impl_->stdin_write_);
    impl_->stdin_write_ = nullptr;
  }
  return static_cast<int>(code);
}

}  // namespace spatial::engine
