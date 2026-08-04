#include "core/utils/fs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "core/errors/project_error.h"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace spatial::core {
namespace fs {
namespace {

std::filesystem::path TempSibling(const std::filesystem::path& path) {
  const auto parent = path.parent_path();
  const auto name = path.filename().string() + ".tmp";
  return parent / name;
}

#if defined(_WIN32)
void SyncDirectory(const std::filesystem::path&) {
  // On Windows, MoveFileEx with WRITE_THROUGH is used for atomic rename
  // durability; a separate directory fsync is not available.
}
#else
void SyncDirectory(const std::filesystem::path& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}
#endif

}  // namespace

void CreateDirectories(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  if (ec) {
    throw StorageError(ErrorCode::kStorageIo,
                       "failed to create directories: " + path.string(), {},
                       false, "Check filesystem permissions.");
  }
}

void AtomicWrite(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
  CreateDirectories(path.parent_path());

#if defined(_WIN32)
  const std::string temp = TempSibling(path).string();
  const HANDLE h = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    throw StorageError(ErrorCode::kStorageIo,
                       "atomic write: cannot create temp file: " + temp, {},
                       true, "Check filesystem permissions.");
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(h, bytes.data(),
                            static_cast<DWORD>(bytes.size()), &written, nullptr);
  const BOOL flush = FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || !flush || written != bytes.size()) {
    RemoveAll(temp);
    throw StorageError(ErrorCode::kStorageAtomicWrite,
                       "atomic write: write/fsync failed for: " + temp, {},
                       true, "Check free disk space.");
  }
  if (!MoveFileExA(temp.c_str(), path.string().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    RemoveAll(temp);
    throw StorageError(ErrorCode::kStorageAtomicWrite,
                       "atomic write: rename failed for: " + path.string(), {},
                       true, "Check free disk space.");
  }
#else
  const std::string temp = TempSibling(path).string();
  const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    throw StorageError(ErrorCode::kStorageIo,
                       "atomic write: cannot open temp file: " + temp, {},
                       true, "Check filesystem permissions.");
  }
  std::size_t offset = 0;
  bool ok = true;
  while (offset < bytes.size()) {
    const auto n = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (n <= 0) {
      ok = false;
      break;
    }
    offset += static_cast<std::size_t>(n);
  }
  const bool flushed = (::fsync(fd) == 0);
  ::close(fd);
  if (!ok || !flushed) {
    RemoveAll(temp);
    throw StorageError(ErrorCode::kStorageAtomicWrite,
                       "atomic write: write/fsync failed for: " + temp, {},
                       true, "Check free disk space.");
  }
  if (::rename(temp.c_str(), path.string().c_str()) != 0) {
    RemoveAll(temp);
    throw StorageError(ErrorCode::kStorageAtomicWrite,
                       "atomic write: rename failed for: " + path.string(), {},
                       true, "Check free disk space.");
  }
  SyncDirectory(path.parent_path());
#endif
}

void AtomicWrite(const std::filesystem::path& path, std::string_view text) {
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  AtomicWrite(path, bytes);
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw StorageError(ErrorCode::kStorageIo,
                       "cannot read file: " + path.string(), {},
                       false, "Check the file exists and is readable.");
  }
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  if (size < 0) {
    throw StorageError(ErrorCode::kStorageIo,
                       "cannot size file: " + path.string());
  }
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in) {
      throw StorageError(ErrorCode::kStorageIo,
                         "cannot read file: " + path.string(), {},
                         false, "The file may be corrupt or truncated.");
    }
  }
  return bytes;
}

std::string ReadText(const std::filesystem::path& path) {
  const auto bytes = ReadFile(path);
  return std::string(bytes.begin(), bytes.end());
}

bool Exists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

void RemoveAll(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

void Rename(const std::filesystem::path& from,
            const std::filesystem::path& to) {
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    throw StorageError(ErrorCode::kStorageIo,
                       "rename failed: " + from.string() + " -> " +
                           to.string(),
                       {}, true, "Check filesystem permissions.");
  }
}

std::vector<std::filesystem::path> ListFiles(
    const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::filesystem::path> ListDirectories(
    const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_directory(ec)) out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string Iso8601UtcNow() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec);
  return buf;
}

std::int64_t TimestampNsNow() {
  const auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             now.time_since_epoch())
      .count();
}

}  // namespace fs

FileLock::FileLock(const std::filesystem::path& path) : path_(path) {
  fs::CreateDirectories(path.parent_path());
#if defined(_WIN32)
  const HANDLE h = CreateFileA(path.string().c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    throw StorageError(ErrorCode::kStorageLock,
                       "cannot open lock file: " + path.string(), {},
                       false, "Check filesystem permissions.");
  }
  OVERLAPPED ov{};
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                  MAXDWORD, MAXDWORD, &ov)) {
    CloseHandle(h);
    throw StorageError(ErrorCode::kStorageLock,
                       "project is locked by another writer: " +
                           path.parent_path().string(),
                       {}, false, "Close the other instance of the project.");
  }
  handle_ = h;
#else
  const int fd = ::open(path.string().c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    throw StorageError(ErrorCode::kStorageLock,
                       "cannot open lock file: " + path.string(), {},
                       false, "Check filesystem permissions.");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    throw StorageError(ErrorCode::kStorageLock,
                       "project is locked by another writer: " +
                           path.parent_path().string(),
                       {}, false, "Close the other instance of the project.");
  }
  fd_ = fd;
#endif
  locked_ = true;
}

FileLock::FileLock(FileLock&& other) noexcept
    : path_(std::move(other.path_)), locked_(other.locked_)
#if defined(_WIN32)
      ,
      handle_(other.handle_)
#else
      ,
      fd_(other.fd_)
#endif
{
  other.locked_ = false;
#if defined(_WIN32)
  other.handle_ = nullptr;
#else
  other.fd_ = -1;
#endif
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    Release();
    path_ = std::move(other.path_);
    locked_ = other.locked_;
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.locked_ = false;
  }
  return *this;
}

FileLock::~FileLock() { Release(); }

void FileLock::Release() {
  if (!locked_) return;
#if defined(_WIN32)
  if (handle_) {
    OVERLAPPED ov{};
    UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &ov);
    CloseHandle(handle_);
    handle_ = nullptr;
  }
#else
  if (fd_ >= 0) {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
  }
#endif
  locked_ = false;
}

}  // namespace spatial::core
