#include "adapters/colmap/colmap_worker.h"

#include <cstdint>
#include <cstring>
#include <exception>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adapters/colmap/colmap_adapter.h"
#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define SPATIAL_WORKER_READ _read
#define SPATIAL_WORKER_WRITE _write
#else
#include <unistd.h>
#define SPATIAL_WORKER_READ ::read
#define SPATIAL_WORKER_WRITE ::write
#endif

namespace spatial::adapters::colmap {
namespace {

using spatial::core::ErrorCode;
using spatial::core::ErrorCodeName;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::Sha256Hex;
using spatial::core::fs::ReadFile;
using nlohmann::json;

bool ReadExact(std::uint8_t* out, std::size_t count) {
  std::size_t got = 0;
  while (got < count) {
    const int r =
        SPATIAL_WORKER_READ(0, out + got, static_cast<unsigned>(count - got));
    if (r <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(r);
  }
  return true;
}

bool WriteExact(const std::uint8_t* data, std::size_t count) {
  std::size_t sent = 0;
  while (sent < count) {
    const int w =
        SPATIAL_WORKER_WRITE(1, data + sent, static_cast<unsigned>(count - sent));
    if (w <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(w);
  }
  return true;
}

std::string ContextText(const std::vector<spatial::core::ErrorContext>& context) {
  std::ostringstream out;
  for (std::size_t i = 0; i < context.size(); ++i) {
    if (i != 0) {
      out << "; ";
    }
    out << context[i].key << '=' << context[i].value;
  }
  return out.str();
}

// Bridges adapter ResultSink calls to framed worker messages, collecting the
// produced artifacts so TaskCompleted can name them (mirrors demo_worker.py).
class BridgeSink : public spatial::adapters::ResultSink {
 public:
  explicit BridgeSink(std::string task_id) : task_id_(std::move(task_id)) {}

  void Progress(int percent, const std::string& stage) override {
    spatial::WorkerMessage msg;
    auto* progress = msg.mutable_task_progress();
    progress->set_task_id(task_id_);
    progress->set_percent(percent);
    progress->set_substage(stage);
    WorkerSendFrame(msg);
  }

  void Log(const std::string& message) override {
    spatial::WorkerMessage msg;
    auto* log = msg.mutable_task_log();
    log->set_task_id(task_id_);
    log->set_message(message);
    WorkerSendFrame(msg);
  }

  void ArtifactProduced(const std::string& payload_path,
                        const std::string& manifest_json) override {
    // The host verifies content_hash against the payload and the manifest
    // before ingesting (fail-closed, C1-S1); the worker computes the digest
    // from the payload file it wrote into its workspace.
    const std::vector<std::uint8_t> bytes = ReadFile(payload_path);
    const std::string content_hash = Sha256Hex(bytes);

    std::string type = "sparse_model";
    std::string mime = "application/octet-stream";
    try {
      const json manifest = json::parse(manifest_json);
      if (manifest.contains("type") && manifest["type"].is_string()) {
        type = manifest["type"].get<std::string>();
      }
      if (manifest.contains("mime_type") && manifest["mime_type"].is_string()) {
        mime = manifest["mime_type"].get<std::string>();
      }
    } catch (const json::exception&) {
      // The manifest is validated by the host; defaults keep the frame
      // well-formed for a degenerate manifest.
    }

    spatial::ArtifactInfo info;
    info.set_artifact_id(FormatUuid(GenerateUuid()));
    info.set_content_hash(content_hash);
    info.set_type(type);
    info.set_size(static_cast<std::int64_t>(bytes.size()));
    info.set_mime(mime);
    info.set_manifest_json(manifest_json);
    info.set_payload_path(payload_path);

    spatial::WorkerMessage msg;
    auto* produced = msg.mutable_task_artifact();
    produced->set_task_id(task_id_);
    *produced->mutable_artifact() = info;
    if (WorkerSendFrame(msg)) {
      outputs_.push_back(std::move(info));
    }
  }

  std::vector<spatial::ArtifactInfo> outputs_;

 private:
  std::string task_id_;
};

void SendTaskFailed(const std::string& task_id, const std::string& code,
                    const std::string& message, const std::string& context,
                    bool recoverable, const std::string& suggested_action) {
  spatial::WorkerMessage msg;
  auto* failed = msg.mutable_task_failed();
  failed->set_task_id(task_id);
  failed->mutable_error()->set_code(code);
  failed->mutable_error()->set_message(message);
  failed->mutable_error()->set_context(context);
  failed->mutable_error()->set_recoverable(recoverable);
  failed->mutable_error()->set_suggested_action(suggested_action);
  WorkerSendFrame(msg);
}

}  // namespace

bool WorkerRecvFrame(spatial::WorkerMessage& msg, bool& eof) {
  eof = false;
  std::uint8_t prefix[4] = {0, 0, 0, 0};
  if (!ReadExact(prefix, 4)) {
    eof = true;  // clean EOF (stdin closed / host exited)
    return false;
  }
  const std::uint32_t length =
      static_cast<std::uint32_t>(prefix[0]) |
      (static_cast<std::uint32_t>(prefix[1]) << 8) |
      (static_cast<std::uint32_t>(prefix[2]) << 16) |
      (static_cast<std::uint32_t>(prefix[3]) << 24);
  if (length > kColmapWorkerMaxFrameSize) {
    return false;  // protocol error
  }
  std::string frame(static_cast<std::size_t>(length), '\0');
  if (length > 0 &&
      !ReadExact(reinterpret_cast<std::uint8_t*>(frame.data()), length)) {
    eof = true;
    return false;
  }
  msg.Clear();
  return msg.ParseFromString(frame);
}

bool WorkerSendFrame(const spatial::WorkerMessage& msg) {
  const std::string bytes = msg.SerializeAsString();
  if (bytes.size() > kColmapWorkerMaxFrameSize) {
    return false;
  }
  const std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
  const std::uint8_t prefix[4] = {
      static_cast<std::uint8_t>(length & 0xFF),
      static_cast<std::uint8_t>((length >> 8) & 0xFF),
      static_cast<std::uint8_t>((length >> 16) & 0xFF),
      static_cast<std::uint8_t>((length >> 24) & 0xFF),
  };
  return WriteExact(prefix, 4) &&
         WriteExact(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                    bytes.size());
}

bool WorkerRunTask(const spatial::TaskRequest& request,
                   const std::string& executable,
                   spatial::adapters::process::CancelToken* token) {
  spatial::WorkerMessage accepted;
  accepted.mutable_task_accepted()->set_task_id(request.task_id());
  if (!WorkerSendFrame(accepted)) {
    return false;
  }

  // The adapter plans from the effective stage configuration; the worker
  // mirrors the engine's TaskRequest shape (engine-side header-only type).
  spatial::engine::TaskRequest plan_request;
  plan_request.config_json = request.spec_json();
  plan_request.input_refs.assign(request.input_refs().begin(),
                                 request.input_refs().end());

  // CAS-free context (C1-S4): the host pre-materialized inputs into
  // workspace/inputs/<hash>; the adapter stages them into images/ +
  // calibration.json. Every input kind is `image` for now (D2: images-only
  // E2E; the adapter defaults missing kinds to image).
  auto context = std::make_shared<ExecutionContext>();
  context->workspace = request.workspace();
  context->store = nullptr;
  context->input_refs = plan_request.input_refs;
  context->input_kinds.assign(plan_request.input_refs.size(), "image");
  context->config_json = plan_request.config_json;
  context->cancel = token;

  try {
    ColmapAdapter adapter(executable, context);
    const std::vector<std::string> plan = adapter.CreatePlan(plan_request);
    BridgeSink sink(request.task_id());
    adapter.Execute(plan, sink);

    spatial::WorkerMessage completed;
    auto* task_completed = completed.mutable_task_completed();
    task_completed->set_task_id(request.task_id());
    for (spatial::ArtifactInfo& info : sink.outputs_) {
      *task_completed->add_outputs() = std::move(info);
    }
    return WorkerSendFrame(completed);
  } catch (const spatial::core::ProjectError& e) {
    if (e.code() == ErrorCode::kAdapterProcessCancelled) {
      // Cooperative cancellation convention (D5, mirrors demo_worker.py):
      // TaskProgress(substage="cancelled") then an empty TaskCompleted; the
      // host maps exactly this to kCancelled.
      spatial::WorkerMessage progress;
      auto* p = progress.mutable_task_progress();
      p->set_task_id(request.task_id());
      p->set_percent(100);
      p->set_substage("cancelled");
      if (!WorkerSendFrame(progress)) {
        return false;
      }
      spatial::WorkerMessage cancelled_completed;
      cancelled_completed.mutable_task_completed()->set_task_id(
          request.task_id());
      return WorkerSendFrame(cancelled_completed);
    }
    // Deterministic failure: stable ErrorCodeName code crosses the boundary,
    // never an exception (adding-adapter.md §6). The timeout code is
    // recoverable; the rest are not, matching the adapter's semantics.
    SendTaskFailed(request.task_id(), ErrorCodeName(e.code()), e.message(),
                   ContextText(e.context()), e.recoverable(),
                   e.suggested_action());
    return true;
  } catch (const std::exception& e) {
    SendTaskFailed(request.task_id(), "WORKER_PROTOCOL",
                   "unexpected worker failure: " + std::string(e.what()), {},
                   false, "");
    return true;
  }
}

}  // namespace spatial::adapters::colmap
