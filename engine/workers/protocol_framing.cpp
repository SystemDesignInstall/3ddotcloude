#include "engine/workers/protocol_framing.h"

#include <cstring>

#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/message_lite.h"

namespace spatial::engine {
namespace {

constexpr std::size_t kMaxFrameSize = 1u << 26;  // 64 MiB protocol guard

// Reads exactly `length` bytes from the child's stdout, slicing `timeout_ms`
// across the reads so a stalled worker still trips the deadline.
bool ReadExact(ChildProcess& process, void* out, std::size_t length,
               std::int64_t timeout_ms, bool& eof, std::string& error) {
  auto* bytes = static_cast<std::uint8_t*>(out);
  std::size_t got = 0;
  while (got < length) {
    std::size_t nread = 0;
    const std::int64_t slice =
        timeout_ms > 0 ? (timeout_ms / 2 + 1) : 1;
    if (!process.ReadStdout(bytes + got, length - got, slice, nread, eof,
                            error)) {
      if (eof) {
        return false;
      }
      if (timeout_ms <= 0) {
        return false;
      }
      timeout_ms -= slice;
      if (timeout_ms < 0) {
        timeout_ms = 0;
      }
      continue;
    }
    got += nread;
  }
  return true;
}

}  // namespace

bool WriteFrame(ChildProcess& process, const google::protobuf::Message& msg,
                std::string& error) {
  const std::string bytes = msg.SerializeAsString();
  if (bytes.size() > kMaxFrameSize) {
    error = "write frame: message exceeds protocol limit";
    return false;
  }
  std::string frame;
  frame.reserve(4 + bytes.size());
  const std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
  const std::uint8_t prefix[4] = {
      static_cast<std::uint8_t>(length & 0xFF),
      static_cast<std::uint8_t>((length >> 8) & 0xFF),
      static_cast<std::uint8_t>((length >> 16) & 0xFF),
      static_cast<std::uint8_t>((length >> 24) & 0xFF),
  };
  frame.append(reinterpret_cast<const char*>(prefix), 4);
  frame.append(bytes);
  return process.Write(frame.data(), frame.size(), error);
}

bool ReadFrame(ChildProcess& process, std::int64_t timeout_ms,
               std::string& frame, bool& eof, std::string& error) {
  std::uint8_t prefix[4] = {0, 0, 0, 0};
  if (!ReadExact(process, prefix, 4, timeout_ms, eof, error)) {
    return false;
  }
  const std::uint32_t length =
      static_cast<std::uint32_t>(prefix[0]) |
      (static_cast<std::uint32_t>(prefix[1]) << 8) |
      (static_cast<std::uint32_t>(prefix[2]) << 16) |
      (static_cast<std::uint32_t>(prefix[3]) << 24);
  if (length > kMaxFrameSize) {
    error = "read frame: length prefix exceeds protocol limit";
    eof = false;
    return false;
  }
  frame.resize(length);
  if (length == 0) {
    return true;
  }
  const bool ok = ReadExact(process, frame.data(), length, timeout_ms, eof, error);
  if (!ok) {
    frame.clear();
  }
  return ok;
}

bool TryParseFrame(const std::string& frame, spatial::WorkerMessage& msg) {
  msg.Clear();
  if (!msg.ParseFromString(frame)) {
    return false;
  }
  if (!msg.has_hello() && !msg.has_task_request() && !msg.has_task_accepted() &&
      !msg.has_task_progress() && !msg.has_task_log() &&
      !msg.has_task_artifact() && !msg.has_task_completed() &&
      !msg.has_task_failed() && !msg.has_task_cancelled() &&
      !msg.has_heartbeat() && !msg.has_shutdown()) {
    return false;  // no payload: malformed
  }
  return true;
}

}  // namespace spatial::engine
