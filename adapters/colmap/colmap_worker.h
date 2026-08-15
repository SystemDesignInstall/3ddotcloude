#pragma once

// COLMAP process worker (C1-S4; RFC-0008 §5/§16; plan §1.1, §3.2). A child
// process the host spawns and drives over the framed worker protocol
// ([u32 LE length][protobuf] on stdin/stdout, ADR-012): WorkerHello
// handshake, then one TaskRequest at a time (max_concurrency = 1).
//
// The worker is the protocol bridge only (design §A): per task it binds a
// CAS-free ExecutionContext (store == nullptr — inputs arrive pre-materialized
// in workspace/inputs/<hash>), runs the adapter's plan through the generic
// subprocess runner, and translates adapter events into frames (TaskProgress /
// TaskLog / TaskArtifactProduced) and ProjectError into TaskFailed with the
// stable code (ErrorCodeName). It never touches the CAS, a host database, or
// the scene (ADR-035/038; check_worker_boundary).

#include <cstddef>
#include <cstdint>
#include <string>

#include "generated/worker.pb.h"

namespace spatial::adapters::process {
class CancelToken;
}

namespace spatial::adapters::colmap {

inline constexpr std::int32_t kColmapWorkerProtocolVersion = 1;
inline constexpr std::size_t kColmapWorkerMaxFrameSize = 1u << 26;  // 64 MiB

// Reads one frame from stdin (blocking). Returns false on EOF (eof = true) or
// on a protocol error (truncated / oversized frame; eof = false). Never
// throws.
bool WorkerRecvFrame(spatial::WorkerMessage& msg, bool& eof);

// Writes one framed message to stdout. Returns false on a broken pipe.
bool WorkerSendFrame(const spatial::WorkerMessage& msg);

// Runs one task to a terminal event. Never throws: adapter errors are
// translated to TaskFailed (stable ErrorCodeName code), cooperative
// cancellation to TaskProgress(substage="cancelled") + empty TaskCompleted.
// Returns false only on a write failure (the host went away).
bool WorkerRunTask(const spatial::TaskRequest& request,
                   const std::string& executable,
                   spatial::adapters::process::CancelToken* token);

}  // namespace spatial::adapters::colmap
