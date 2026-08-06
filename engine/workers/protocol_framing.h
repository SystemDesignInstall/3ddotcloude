#pragma once

// Worker IPC framing (ADR-012, worker-protocol §1): every message is
// [u32 little-endian length][protobuf bytes] over the child's stdio. Length
// is the serialized byte count, excluding the 4-byte prefix. A frame is
// never split or coalesced at the boundary.

#include <cstdint>
#include <string>

#include "engine/workers/child_process.h"
#include "generated/worker.pb.h"

namespace spatial::engine {

// Serializes `msg` and writes it to the child's stdin. Returns false on a
// broken pipe (child exited) with `error` set.
bool WriteFrame(ChildProcess& process, const google::protobuf::Message& msg,
                std::string& error);

// Reads exactly one frame from the child's stdout with a deadline. On
// success `frame` holds the raw payload bytes and true is returned. On
// failure false is returned: `eof` is true when the stream ended (child
// exited / closed stdout), false on a pure timeout.
bool ReadFrame(ChildProcess& process, std::int64_t timeout_ms,
               std::string& frame, bool& eof, std::string& error);

// Parses raw frame bytes into the protocol wrapper. Returns false on a
// malformed frame (protocol error; the caller terminates the worker).
bool TryParseFrame(const std::string& frame, spatial::WorkerMessage& msg);

}  // namespace spatial::engine
