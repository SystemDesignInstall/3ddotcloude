#pragma once

// Filesystem helpers for the storage layer (ADR-008, ADR-009, ADR-010).
// Enforce the atomic temp-then-rename write pattern and platform-native
// project locking. Cross-platform: Windows + POSIX.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace spatial::core {

namespace fs {

// Creates all parent directories of `path`. Idempotent.
void CreateDirectories(const std::filesystem::path& path);

// Atomic write: writes to a temp file in the same directory, fsyncs the
// file, then renames it over `path`. On failure the temp file is removed
// and StorageError is thrown. Throws StorageReadOnly if the target exists
// and is not writable.
void AtomicWrite(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes);
void AtomicWrite(const std::filesystem::path& path, std::string_view text);

// Reads an entire file. Throws StorageError on I/O failure.
std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path);
std::string ReadText(const std::filesystem::path& path);

// True if the path exists (files and directories).
bool Exists(const std::filesystem::path& path);

// Removes a file or directory tree. Idempotent; missing paths are not errors.
void RemoveAll(const std::filesystem::path& path);

// Renames `from` to `to`; both must be on the same filesystem.
void Rename(const std::filesystem::path& from, const std::filesystem::path& to);

// Lists regular files directly under `dir` (not recursive), sorted.
std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& dir);

// Lists directories directly under `dir` (not recursive), sorted.
std::vector<std::filesystem::path> ListDirectories(
    const std::filesystem::path& dir);

// Current wall-clock time as ISO-8601 UTC ("2026-08-04T10:15:00Z").
std::string Iso8601UtcNow();

// Wall-clock time as nanoseconds since Unix epoch (TimestampNs, ADR-007).
std::int64_t TimestampNsNow();

}  // namespace fs

// Platform-native exclusive lock on a lock file. Holds the lock until
// destruction. On POSIX the file is locked with flock; on Windows with
// LockFileEx. Used for single-writer project semantics (ADR-008).
class FileLock {
 public:
  // Acquires an exclusive lock on `path`, creating the file if needed.
  // Throws StorageError if the lock cannot be acquired (concurrent writer).
  explicit FileLock(const std::filesystem::path& path);
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  ~FileLock();

  bool locked() const noexcept { return locked_; }
  void Release();

 private:
  std::filesystem::path path_;
  bool locked_ = false;
#if defined(_WIN32)
  void* handle_ = nullptr;  // HANDLE
#else
  int fd_ = -1;
#endif
};

}  // namespace spatial::core
