// COLMAP process worker entrypoint (C1-S4; plan §1.1, §3.2; design §A).
//
// Spawned by the host (ProcessExecutor) with the COLMAP executable path as
// argv[1]. Sends WorkerHello (protocol_version 1, capabilities/resources from
// the adapter descriptor, max_concurrency 1), then loops over framed messages:
// one TaskRequest at a time, TaskCancelled sets the cooperative token the
// adapter checks at every stage boundary, Shutdown exits.
//
// Layout mirrors demo_worker.py: a dedicated stdin reader keeps consuming
// frames while the (synchronous, single-flight) task runs, so cancellation
// frames arrive mid-task; a heartbeater keeps the host's liveness window fed.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "adapters/colmap/colmap_adapter.h"
#include "adapters/colmap/colmap_worker.h"
#include "adapters/process/process_runner.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace spatial::adapters::colmap {
namespace {

using spatial::adapters::process::CancelToken;

struct SharedState {
  std::mutex m;
  std::condition_variable cv;
  std::optional<spatial::TaskRequest> incoming;
  CancelToken* active_cancel = nullptr;
  std::string active_task_id;
  std::atomic<bool> shutdown{false};
};

void SetBinaryStdio() {
#if defined(_WIN32)
  // The protocol is byte-exact: the CRT must not translate newlines or treat
  // 0x1A (Ctrl-Z) as EOF on the inherited pipe handles (worker-protocol §1).
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
}

void SendHello(const std::string& executable) {
  const ColmapAdapter descriptor_adapter(executable);
  const spatial::adapters::AdapterDescriptor descriptor =
      descriptor_adapter.Descriptor();

  spatial::WorkerMessage hello;
  auto* h = hello.mutable_hello();
  h->set_protocol_version(kColmapWorkerProtocolVersion);
  h->set_worker_id(
      spatial::core::FormatUuid(spatial::core::GenerateUuid()));
  auto* capabilities = h->mutable_capabilities();
  for (const std::string& capability : descriptor.capabilities) {
    capabilities->add_capabilities(capability);
  }
  auto* resources = capabilities->mutable_resources();
  resources->set_cpu_cores(descriptor.profile.capacity.cores);
  resources->set_ram_mb(
      static_cast<std::int64_t>(descriptor.profile.capacity.ram_bytes) /
      (1024 * 1024));
  resources->set_gpu_mem_mb(0);
  resources->set_os("unknown");
  resources->set_arch("unknown");
  capabilities->set_max_concurrency(descriptor.profile.max_concurrency);
  WorkerSendFrame(hello);
}

void ReaderLoop(SharedState& state) {
  for (;;) {
    spatial::WorkerMessage msg;
    bool eof = false;
    if (!WorkerRecvFrame(msg, eof)) {
      // EOF or a protocol error: the host is gone / never coming back.
      state.shutdown.store(true, std::memory_order_relaxed);
      state.cv.notify_all();
      return;
    }
    if (msg.has_task_request()) {
      std::lock_guard<std::mutex> lock(state.m);
      state.incoming = msg.task_request();
      state.cv.notify_all();
    } else if (msg.has_task_cancelled()) {
      const std::string target = msg.task_cancelled().task_id();
      std::lock_guard<std::mutex> lock(state.m);
      if (state.active_cancel != nullptr &&
          state.active_task_id == target) {
        state.active_cancel->Cancel();
      }
    } else if (msg.has_shutdown()) {
      state.shutdown.store(true, std::memory_order_relaxed);
      state.cv.notify_all();
      return;
    }
    // Unknown payloads (hello, heartbeats, ...) are ignored (worker-protocol
    // §3).
  }
}

void HeartbeatLoop(const SharedState& state) {
  while (!state.shutdown.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    spatial::WorkerMessage msg;
    msg.mutable_heartbeat()->set_timestamp_ns(
        spatial::core::fs::TimestampNsNow());
    WorkerSendFrame(msg);
  }
}

}  // namespace
}  // namespace spatial::adapters::colmap

int main(int argc, char** argv) {
  using spatial::adapters::colmap::kColmapWorkerProtocolVersion;
  using spatial::adapters::colmap::SharedState;
  using spatial::adapters::colmap::WorkerRunTask;
  using spatial::adapters::colmap::WorkerSendFrame;

  if (argc < 2) {
    std::fputs("usage: colmap_worker <colmap-executable> [--version]\n",
               stderr);
    return 64;
  }
  const std::string executable = argv[1];
  if (argc >= 3 && std::string(argv[2]) == "--version") {
    std::printf("colmap_worker %s (protocol %d)\n", "0.1.0",
                kColmapWorkerProtocolVersion);
    return 0;
  }

  spatial::adapters::colmap::SetBinaryStdio();

  SharedState state;
  spatial::adapters::colmap::SendHello(executable);

  std::thread reader(
      [&state]() { spatial::adapters::colmap::ReaderLoop(state); });
  std::thread heartbeater(
      [&state]() { spatial::adapters::colmap::HeartbeatLoop(state); });

  while (!state.shutdown.load(std::memory_order_relaxed)) {
    std::optional<spatial::TaskRequest> request;
    {
      std::unique_lock<std::mutex> lock(state.m);
      state.cv.wait_for(lock, std::chrono::milliseconds(200), [&state]() {
        return state.incoming.has_value() ||
               state.shutdown.load(std::memory_order_relaxed);
      });
      if (state.incoming) {
        request = std::move(state.incoming);
        state.incoming.reset();
      }
    }
    if (!request) {
      continue;
    }

    spatial::adapters::process::CancelToken token;
    {
      std::lock_guard<std::mutex> lock(state.m);
      state.active_cancel = &token;
      state.active_task_id = request->task_id();
    }
    WorkerRunTask(*request, executable, &token);
    {
      std::lock_guard<std::mutex> lock(state.m);
      state.active_cancel = nullptr;
      state.active_task_id.clear();
    }
  }

  state.shutdown.store(true, std::memory_order_relaxed);
  reader.join();
  if (heartbeater.joinable()) {
    heartbeater.join();
  }
  return 0;
}
