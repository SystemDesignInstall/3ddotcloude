// Windows implementation of the generic adapter-layer process runner
// (C1-S3; plan §9). Spawns via CreateProcessW with anonymous pipes for
// stdout/stderr; two reader threads drain the pipes to completion so output
// can be captured in full without deadlocking on a full pipe buffer. The
// caller's deadline and cancel token are honored by polling WaitForSingleObject
// in small increments; on timeout or cancellation the child is force-
// terminated (TerminateProcess) and reaped before returning.

#include "adapters/process/process_runner.h"

#ifndef _WIN32
#error process_runner_win32.cpp must only be built on Windows
#endif

#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cwchar>
#include <map>
#include <sstream>
#include <thread>

namespace spatial::adapters::process {
namespace {

std::string WinErrorText(DWORD code) {
  LPWSTR buffer = nullptr;
  const DWORD len = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string text;
  if (len > 0 && buffer != nullptr) {
    const int need = WideCharToMultiByte(CP_UTF8, 0, buffer,
                                         static_cast<int>(len), nullptr, 0,
                                         nullptr, nullptr);
    if (need > 0) {
      text.resize(need);
      WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(len),
                          text.data(), need, nullptr, nullptr);
    }
    LocalFree(buffer);
  }
  if (text.empty()) {
    std::ostringstream os;
    os << "error " << code;
    text = os.str();
  }
  while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
    text.pop_back();
  }
  return text;
}

// Merges "KEY=VALUE" overrides over the parent environment (CreateProcessW
// replaces the whole block, so the parent entries must be re-added).
std::wstring BuildEnvBlock(const std::vector<std::string>& overrides) {
  // Small case-insensitive key map for Win32 environment variables.
  struct KeyLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
      return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
  };
  std::map<std::wstring, std::wstring, KeyLess> merged;
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

struct PipeEnds {
  HANDLE read_ = nullptr;
  HANDLE write_ = nullptr;
  ~PipeEnds();
};
PipeEnds::~PipeEnds() {
  if (read_) CloseHandle(read_);
  if (write_) CloseHandle(write_);
}

bool MakePipe(PipeEnds& p, SECURITY_ATTRIBUTES& sa) {
  HANDLE r = nullptr, w = nullptr;
  if (!CreatePipe(&r, &w, &sa, 0)) {
    return false;
  }
  p.read_ = r;
  p.write_ = w;
  return true;
}

bool DontInherit(HANDLE h, std::string& error) {
  if (!SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0)) {
    error = "SetHandleInformation failed: " + WinErrorText(GetLastError());
    return false;
  }
  return true;
}

std::wstring BuildCommandLine(const std::vector<std::string>& argv) {
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
  return command_line;
}

// Drains one pipe into `out` until EOF. ReadFile on an anonymous pipe blocks
// until data is available (or the write end is closed); a FALSE return means
// the child closed its end (or exited), which terminates the capture.
void DrainPipe(HANDLE h, std::string& out) {
  char buffer[65536];
  DWORD got = 0;
  while (ReadFile(h, buffer, static_cast<DWORD>(sizeof(buffer)), &got,
                  nullptr) &&
         got > 0) {
    out.append(buffer, got);
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

  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  PipeEnds stdout_pipe;
  PipeEnds stderr_pipe;
  if (!MakePipe(stdout_pipe, sa) || !MakePipe(stderr_pipe, sa)) {
    result.error_message =
        "run: CreatePipe failed: " + WinErrorText(GetLastError());
    return result;
  }
  if (!DontInherit(stdout_pipe.read_, result.error_message) ||
      !DontInherit(stderr_pipe.read_, result.error_message)) {
    return result;
  }

  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = nullptr;  // the child's stdin is closed
  si.hStdOutput = stdout_pipe.write_;
  si.hStdError = stderr_pipe.write_;

  std::wstring command_line = BuildCommandLine(spec.argv);
  std::wstring env_block;
  LPVOID env_ptr = nullptr;
  if (!spec.env.empty()) {
    env_block = BuildEnvBlock(spec.env);
    env_ptr = env_block.data();
  }
  std::wstring cwd;
  LPCWSTR cwd_ptr = nullptr;
  if (!spec.working_directory.empty()) {
    cwd = spec.working_directory.wstring();
    cwd_ptr = cwd.c_str();
  }

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  const BOOL ok = CreateProcessW(
      nullptr, &command_line[0], nullptr, nullptr, /*bInheritHandles=*/TRUE,
      CREATE_UNICODE_ENVIRONMENT, env_ptr, cwd_ptr, &si, &pi);
  if (!ok) {
    result.error_message =
        "run: CreateProcess failed: " + WinErrorText(GetLastError());
    return result;
  }
  CloseHandle(pi.hThread);

  // The child owns its write ends; close them in the parent so EOF arrives
  // once the child exits.
  CloseHandle(stdout_pipe.write_);
  CloseHandle(stderr_pipe.write_);
  stdout_pipe.write_ = nullptr;
  stderr_pipe.write_ = nullptr;

  std::string stdout_text;
  std::string stderr_text;
  std::thread stdout_thread(DrainPipe, stdout_pipe.read_, std::ref(stdout_text));
  std::thread stderr_thread(DrainPipe, stderr_pipe.read_, std::ref(stderr_text));

  const auto start = std::chrono::steady_clock::now();
  bool timed_out = false;
  bool cancelled = false;
  for (;;) {
    const DWORD wait = WaitForSingleObject(pi.hProcess, 25);
    if (wait == WAIT_OBJECT_0) {
      break;  // the child exited
    }
    if (cancel != nullptr && cancel->cancelled()) {
      cancelled = true;
      TerminateProcess(pi.hProcess, 1u);
      WaitForSingleObject(pi.hProcess, INFINITE);
      break;
    }
    if (timeout_ms >= 0) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed >= timeout_ms) {
        timed_out = true;
        TerminateProcess(pi.hProcess, 1u);
        WaitForSingleObject(pi.hProcess, INFINITE);
        break;
      }
    }
  }

  stdout_thread.join();
  stderr_thread.join();
  CloseHandle(stdout_pipe.read_);
  CloseHandle(stderr_pipe.read_);
  stdout_pipe.read_ = nullptr;
  stderr_pipe.read_ = nullptr;

  if (timed_out || cancelled) {
    result.outcome = timed_out ? ProcessOutcome::kTimedOut
                               : ProcessOutcome::kCancelled;
    result.exit_code = -1;
    result.stdout_text = std::move(stdout_text);
    result.stderr_text = std::move(stderr_text);
    result.error_message = timed_out ? "child process exceeded the time limit"
                                     : "child process was cancelled";
  } else {
    DWORD code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &code)) {
      code = 1;
    }
    result.outcome = ProcessOutcome::kCompleted;
    result.exit_code = static_cast<int>(code);
    result.stdout_text = std::move(stdout_text);
    result.stderr_text = std::move(stderr_text);
  }
  CloseHandle(pi.hProcess);
  return result;
}

}  // namespace spatial::adapters::process
