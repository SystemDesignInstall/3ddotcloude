#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/errors/project_error.h"
#include "core/storage/metadata_db.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"
#include "engine/cache/task_cache.h"
#include "engine/scheduler/scheduler.h"
#include "engine/scheduler/scheduler_state_store.h"
#include "engine/workers/child_process.h"
#include "engine/workers/process_executor.h"
#include "engine/workers/worker_handle.h"
#include "tests/unit/engine_test_helpers.h"
#include "worker_paths.h"

namespace spatial::engine {
namespace {

// Env lookup that avoids the MSVC C4996 deprecation warning for getenv.
const char* GetEnv(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  size_t len = 0;
  if (_dupenv_s(&value, &len, name) == 0 && value != nullptr) {
    static thread_local std::string storage;
    storage = value;
    free(value);
    return storage.c_str();
  }
  return nullptr;
#else
  return std::getenv(name);
#endif
}

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactStore;
using spatial::core::GenerateUuid;
using spatial::core::MetadataDb;
using spatial::core::Sha256Hex;
using spatial::core::WorkerError;

// Detects a usable Python interpreter ("python", then the "py -3" launcher)
// and returns the argv prefix; empty when unavailable.
std::vector<std::string> PythonPrefix() {
  const char* env = GetEnv("SPATIAL_TEST_PYTHON");
  if (env != nullptr && *env != '\0') {
    return {env};
  }
  std::string error;
  auto child = ChildProcess::Spawn({"python", "--version"}, error);
  if (child != nullptr) {
    child->Wait();
    return {"python"};
  }
  child = ChildProcess::Spawn({"py", "-3", "--version"}, error);
  if (child != nullptr) {
    child->Wait();
    return {"py", "-3"};
  }
  return {};
}

TaskRequest MakeRequest() {
  TaskRequest req;
  req.task_id = GenerateUuid();
  req.task_type = "demo_task";
  req.config_json = R"({"input":"hello"})";
  req.workspace = "temp/job/task";
  return req;
}

std::vector<WorkerEvent> DrainToTerminal(ProcessExecutor& executor,
                                         std::int64_t timeout_events_ms) {
  std::vector<WorkerEvent> events;
  WorkerEvent event;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (executor.WaitForEvent(event, timeout_events_ms)) {
      events.push_back(event);
      if (event.type == WorkerEventType::kCompleted ||
          event.type == WorkerEventType::kFailed ||
          event.type == WorkerEventType::kCancelled) {
        break;
      }
    }
  }
  return events;
}

class ProcessExecutorTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    python_ = PythonPrefix();
    if (python_.empty()) {
      GTEST_SKIP() << "no Python interpreter available";
    }
  }

  std::unique_ptr<ProcessExecutor> MakeExecutor(
      const std::vector<std::string>& extra_argv = {},
      spatial::core::ArtifactStore* store = nullptr) {
    std::vector<std::string> command = python_;
    command.push_back(kDemoWorkerScript);
    command.insert(command.end(), extra_argv.begin(), extra_argv.end());
    return std::make_unique<ProcessExecutor>(test::BigWorker(), command,
                                             kWorkerGeneratedProtoDir, 5000,
                                             store);
  }

  // Directory containing spatial_wire.py (imported by the fixture workers).
  static std::string PythonWireDir() {
    return std::filesystem::path(kDemoWorkerScript).parent_path().string();
  }

  static std::vector<std::string> python_;
};

std::vector<std::string> ProcessExecutorTest::python_;

TEST_F(ProcessExecutorTest, HandshakeNegotiatesCapabilities) {
  auto executor = MakeExecutor();
  const auto& profile = executor->profile();
  ASSERT_NE(profile.capabilities.end(),
            std::find(profile.capabilities.begin(), profile.capabilities.end(),
                      "demo_task"));
  EXPECT_GE(profile.max_concurrency, 1);
  EXPECT_FALSE(spatial::core::IsNil(executor->id()));
}

TEST_F(ProcessExecutorTest, EndToEndTaskWithArtifact) {
  auto executor = MakeExecutor();
  executor->Submit(MakeRequest());

  const auto events = DrainToTerminal(*executor, 2000);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().type, WorkerEventType::kCompleted);

  bool saw_progress = false;
  bool saw_artifact = false;
  for (const auto& e : events) {
    if (e.type == WorkerEventType::kProgress) {
      saw_progress = true;
    }
    if (e.type == WorkerEventType::kArtifactProduced) {
      saw_artifact = true;
      // SHA-256 hex from the worker's payload.
      EXPECT_EQ(e.artifact_ref.size(), 64u);
    }
  }
  EXPECT_TRUE(saw_progress);
  EXPECT_TRUE(saw_artifact);
}

TEST_F(ProcessExecutorTest, CancelMidTaskReportsCancelled) {
  auto executor = MakeExecutor();

  const auto req = MakeRequest();
  executor->Submit(req);

  // Wait until the task is actually running (progress seen).
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  WorkerEvent event;
  bool saw_progress = false;
  while (std::chrono::steady_clock::now() < deadline && !saw_progress) {
    if (executor->WaitForEvent(event, 2000)) {
      if (event.type == WorkerEventType::kProgress) {
        saw_progress = true;
      }
    }
  }
  ASSERT_TRUE(saw_progress);

  executor->Cancel(req.task_id, "user aborted");
  const auto rest = DrainToTerminal(*executor, 2000);
  ASSERT_FALSE(rest.empty());
  EXPECT_EQ(rest.back().type, WorkerEventType::kCancelled);
}

TEST_F(ProcessExecutorTest, CrashAfterHelloSurfacesWorkerCrashed) {
  // A fixture worker that sends WorkerHello and then dies without ever
  // acknowledging a task: EOF must surface as WORKER_CRASHED (recoverable).
  const auto script = std::filesystem::temp_directory_path() /
                      ("spatial_crash_worker_" +
                       std::to_string(std::time(nullptr)) + ".py");
  {
    std::ofstream out(script);
    ASSERT_TRUE(out) << script;
    out << "import os, struct, sys, uuid\n"
           "sys.path.insert(0, r'" + PythonWireDir() + "')\n"
           "import spatial_wire\n"
           "def write(p):\n"
           "    sys.stdout.buffer.write(struct.pack('<I', len(p)) + p)\n"
           "    sys.stdout.buffer.flush()\n"
           "write(spatial_wire.worker_hello(1, str(uuid.uuid4()),\n"
           "    ['demo_task'], 2, 1024, 0, 'win32', '64-bit', 1))\n"
           "os._exit(3)\n";
  }

  std::vector<std::string> command = python_;
  command.push_back(script.string());
  ProcessExecutor executor(test::BigWorker(), command,
                           kWorkerGeneratedProtoDir);

  executor.Submit(MakeRequest());
  WorkerEvent event;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  bool saw_crash = false;
  while (std::chrono::steady_clock::now() < deadline && !saw_crash) {
    if (executor.WaitForEvent(event, 2000)) {
      if (event.type == WorkerEventType::kFailed) {
        saw_crash = true;
      }
    }
  }
  ASSERT_TRUE(saw_crash);
  EXPECT_EQ(event.error_code, "WORKER_CRASHED");
  EXPECT_TRUE(event.recoverable);

  std::error_code ec;
  std::filesystem::remove(script, ec);
}

TEST_F(ProcessExecutorTest, StalledWorkerTripsHeartbeatTimeout) {
  // A fixture worker that responds to a task with one progress report then
  // goes silent: WaitForEvent must time out (return false) so the scheduler
  // can apply its heartbeat policy.
  const auto script = std::filesystem::temp_directory_path() /
                      ("spatial_stall_worker_" +
                       std::to_string(std::time(nullptr)) + ".py");
  {
    std::ofstream out(script);
    ASSERT_TRUE(out) << script;
    out << "import struct, sys, time, uuid\n"
           "sys.path.insert(0, r'" + PythonWireDir() + "')\n"
           "import spatial_wire\n"
           "def write(p):\n"
           "    sys.stdout.buffer.write(struct.pack('<I', len(p)) + p)\n"
           "    sys.stdout.buffer.flush()\n"
           "write(spatial_wire.worker_hello(1, str(uuid.uuid4()),\n"
           "    ['demo_task'], 2, 1024, 0, 'win32', '64-bit', 1))\n"
           "prefix = sys.stdin.buffer.read(4)\n"
           "if prefix:\n"
           "    (length,) = struct.unpack('<I', prefix)\n"
           "    sys.stdin.buffer.read(length)\n"
           "    write(spatial_wire.task_progress('', 50))\n"
           "time.sleep(15)\n";
  }

  std::vector<std::string> command = python_;
  command.push_back(script.string());
  ProcessExecutor executor(test::BigWorker(), command,
                           kWorkerGeneratedProtoDir);

  executor.Submit(MakeRequest());

  // First event: the worker's single progress report.
  WorkerEvent event;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  bool saw_progress = false;
  while (std::chrono::steady_clock::now() < deadline && !saw_progress) {
    if (executor.WaitForEvent(event, 2000)) {
      if (event.type == WorkerEventType::kProgress) {
        saw_progress = true;
      }
    }
  }
  ASSERT_TRUE(saw_progress);

  // The worker now stalls for 15 s: a 500 ms read must time out.
  EXPECT_FALSE(executor.WaitForEvent(event, 500));

  executor.Shutdown();  // terminates the stalled child

  std::error_code ec;
  std::filesystem::remove(script, ec);
}

// C++ scheduler <-> Python worker end-to-end (RFC-0003 P1.3 integration).
TEST_F(ProcessExecutorTest, SchedulerRunsDemoWorkerEndToEnd) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("spatial_e2e_" + std::to_string(std::time(nullptr)) + "_" +
                     std::to_string(std::rand()));
  std::filesystem::create_directories(root);
  {
    auto db = std::make_unique<MetadataDb>(MetadataDb::Create(root / "p.db"));
    ArtifactStore artifacts(root / "artifacts", *db);
    SchedulerStateStore state(*db);
    TaskCache cache(artifacts, state, "producer-v1", "commit-abc");
    auto executor = MakeExecutor();
    Scheduler scheduler(state, cache, *executor);

    TaskGraph graph(GenerateUuid());
    graph.AddTask(test::MakeTask("demo_task", {}, {"out"}));
    scheduler.Run(graph);
    const auto task = *state.FindTask(graph.Order().front());
    EXPECT_EQ(task.state, TaskStatus::kSucceeded);
    EXPECT_EQ(task.outputs.size(), 1u);
    EXPECT_EQ(task.outputs[0].size(), 64u);
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

// --- C1-S1: fail-closed CAS ingest + input materialization -----------------
//
// A fixture worker producing an artifact with a constant payload. The
// trailing `behavior` lines customize the artifact report (bad hash, missing
// payload, ...). All paths are absolute so the executor can read the payload.
std::string IngestFixtureScript(const std::string& wire_dir,
                                const std::string& behavior) {
  return "import hashlib, json, os, struct, sys, uuid\n"
         "sys.path.insert(0, r'" + wire_dir + "')\n"
         "import spatial_wire\n"
         "def write(p):\n"
         "    sys.stdout.buffer.write(struct.pack('<I', len(p)) + p)\n"
         "    sys.stdout.buffer.flush()\n"
         "def read_frame():\n"
         "    prefix = sys.stdin.buffer.read(4)\n"
         "    if not prefix: return None\n"
         "    (length,) = struct.unpack('<I', prefix)\n"
         "    return sys.stdin.buffer.read(length)\n"
         "write(spatial_wire.worker_hello(1, str(uuid.uuid4()),\n"
         "    ['demo_task'], 2, 1024, 0, 'win32', '64-bit', 1))\n"
         "frame = read_frame()\n"
         "if frame is None: sys.exit(0)\n"
         "name, payload = spatial_wire.parse_worker_message(frame)\n"
         "req = spatial_wire.parse_task_request(payload)\n"
         "data = b'constant-payload-bytes'\n"
         "content_hash = hashlib.sha256(data).hexdigest()\n"
         "artifact_id = str(uuid.uuid4())\n"
         "manifest = json.dumps({\n"
         "    'artifact_uuid': artifact_id,\n"
         "    'content_hash': content_hash,\n"
         "    'type': 'demo_output',\n"
         "    'schema_version': 1,\n"
         "    'producer': {'id': 'fixture', 'version': '1.0.0', 'git_commit': 'abc'},\n"
         "    'mime_type': 'application/octet-stream',\n"
         "})\n"
         "path = os.path.join(req['workspace'], 'out.bin')\n"
         "os.makedirs(req['workspace'], exist_ok=True)\n"
         "with open(path, 'wb') as fh:\n"
         "    fh.write(data)\n"
         + behavior + "\n";
}

// Writes a fixture worker script and returns its path.
std::filesystem::path WriteFixtureWorker(const std::string& tag,
                                         const std::string& script) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("spatial_" + tag + "_" +
                     std::to_string(std::time(nullptr)) + "_" +
                     std::to_string(std::rand()) + ".py");
  std::ofstream out(path);
  out << script;
  return path;
}

TEST_F(ProcessExecutorTest, IngestSucceedsPayloadLandsInCasAndDedupes) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("spatial_ingest_" + std::to_string(std::time(nullptr)) +
                     "_" + std::to_string(std::rand()));
  std::filesystem::create_directories(root);
  {
    auto db = MetadataDb::Create(root / "p.db");
    ArtifactStore store(root / "artifacts", db);
    const std::string content =
        std::string("constant-payload-bytes");
    const std::string expected_hash =
        Sha256Hex(std::vector<std::uint8_t>(content.begin(), content.end()));

    const auto run = [&](const std::filesystem::path& ws) {
      const auto script =
          WriteFixtureWorker("ingest_ok",
                             IngestFixtureScript(PythonWireDir(), R"(write(
    spatial_wire.task_artifact(req['task_id'], artifact_id, content_hash,
        'demo_output', len(data), 'application/octet-stream', manifest, path))
write(spatial_wire.task_completed(req['task_id'], []))
)"));
      std::vector<std::string> command = python_;
      command.push_back(script.string());
      ProcessExecutor executor(test::BigWorker(), command,
                               kWorkerGeneratedProtoDir, 5000, &store);

      TaskRequest req = MakeRequest();
      req.workspace = ws.string();
      executor.Submit(req);
      const auto events = DrainToTerminal(executor, 2000);
      EXPECT_EQ(events.back().type, WorkerEventType::kCompleted);
      for (const auto& e : events) {
        if (e.type == WorkerEventType::kArtifactProduced) {
          EXPECT_EQ(e.artifact_ref, expected_hash);
          // The additive WorkerEvent fields carry the worker report.
          EXPECT_FALSE(e.payload_path.empty());
          EXPECT_FALSE(e.manifest_json.empty());
        }
      }
      std::error_code ec;
      std::filesystem::remove(script, ec);
    };

    run(root / "ws1");
    EXPECT_TRUE(store.Has(expected_hash));
    EXPECT_EQ(store.PayloadCount(), 1u);

    // Repeated ingest of identical content deduplicates (no new payload).
    run(root / "ws2");
    EXPECT_EQ(store.PayloadCount(), 1u);
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_F(ProcessExecutorTest, IngestFailClosedNoPayloadPath) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("spatial_ingest_" + std::to_string(std::time(nullptr)) +
                     "_" + std::to_string(std::rand()));
  std::filesystem::create_directories(root);
  {
    auto db = MetadataDb::Create(root / "p.db");
    ArtifactStore store(root / "artifacts", db);
    const auto script = WriteFixtureWorker(
        "ingest_nopath",
        IngestFixtureScript(PythonWireDir(), R"(write(
    spatial_wire.task_artifact(req['task_id'], artifact_id, content_hash,
        'demo_output', len(data), 'application/octet-stream', manifest, ''))
write(spatial_wire.task_completed(req['task_id'], []))
)"));
    std::vector<std::string> command = python_;
    command.push_back(script.string());
    ProcessExecutor executor(test::BigWorker(), command, kWorkerGeneratedProtoDir,
                             5000, &store);

    TaskRequest req = MakeRequest();
    req.workspace = (root / "ws").string();
    executor.Submit(req);
    const auto events = DrainToTerminal(executor, 2000);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, WorkerEventType::kFailed);
    EXPECT_EQ(events.back().error_code, "WORKER_INGEST_FAILED");
    EXPECT_FALSE(events.back().error_message.empty());
    EXPECT_EQ(store.PayloadCount(), 0u);
    std::error_code ec;
    std::filesystem::remove(script, ec);
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_F(ProcessExecutorTest, IngestFailClosedMissingPayloadFile) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("spatial_ingest_" + std::to_string(std::time(nullptr)) +
                     "_" + std::to_string(std::rand()));
  std::filesystem::create_directories(root);
  {
    auto db = MetadataDb::Create(root / "p.db");
    ArtifactStore store(root / "artifacts", db);
    const auto script = WriteFixtureWorker(
        "ingest_missing",
        IngestFixtureScript(PythonWireDir(), R"(write(
    spatial_wire.task_artifact(req['task_id'], artifact_id, content_hash,
        'demo_output', len(data), 'application/octet-stream', manifest,
        os.path.join(req['workspace'], 'does-not-exist.bin')))
write(spatial_wire.task_completed(req['task_id'], []))
)"));
    std::vector<std::string> command = python_;
    command.push_back(script.string());
    ProcessExecutor executor(test::BigWorker(), command, kWorkerGeneratedProtoDir,
                             5000, &store);

    TaskRequest req = MakeRequest();
    req.workspace = (root / "ws").string();
    executor.Submit(req);
    const auto events = DrainToTerminal(executor, 2000);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, WorkerEventType::kFailed);
    EXPECT_EQ(events.back().error_code, "WORKER_INGEST_FAILED");
    EXPECT_EQ(store.PayloadCount(), 0u);
    std::error_code ec;
    std::filesystem::remove(script, ec);
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_F(ProcessExecutorTest, IngestFailClosedHashMismatch) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("spatial_ingest_" + std::to_string(std::time(nullptr)) +
                     "_" + std::to_string(std::rand()));
  std::filesystem::create_directories(root);
  {
    auto db = MetadataDb::Create(root / "p.db");
    ArtifactStore store(root / "artifacts", db);
    const auto script = WriteFixtureWorker(
        "ingest_badhash",
        IngestFixtureScript(PythonWireDir(), R"(write(
    spatial_wire.task_artifact(req['task_id'], artifact_id, '0' * 64,
        'demo_output', len(data), 'application/octet-stream', manifest, path))
write(spatial_wire.task_completed(req['task_id'], []))
)"));
    std::vector<std::string> command = python_;
    command.push_back(script.string());
    ProcessExecutor executor(test::BigWorker(), command, kWorkerGeneratedProtoDir,
                             5000, &store);

    TaskRequest req = MakeRequest();
    req.workspace = (root / "ws").string();
    executor.Submit(req);
    const auto events = DrainToTerminal(executor, 2000);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, WorkerEventType::kFailed);
    EXPECT_EQ(events.back().error_code, "WORKER_INGEST_FAILED");
    EXPECT_EQ(store.PayloadCount(), 0u);
    std::error_code ec;
    std::filesystem::remove(script, ec);
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_F(ProcessExecutorTest, IngestFailClosedMalformedManifest) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("spatial_ingest_" + std::to_string(std::time(nullptr)) +
                     "_" + std::to_string(std::rand()));
  std::filesystem::create_directories(root);
  {
    auto db = MetadataDb::Create(root / "p.db");
    ArtifactStore store(root / "artifacts", db);
    const auto script = WriteFixtureWorker(
        "ingest_badmanifest",
        IngestFixtureScript(PythonWireDir(), R"(write(
    spatial_wire.task_artifact(req['task_id'], artifact_id, content_hash,
        'demo_output', len(data), 'application/octet-stream',
        'not-json', path))
write(spatial_wire.task_completed(req['task_id'], []))
)"));
    std::vector<std::string> command = python_;
    command.push_back(script.string());
    ProcessExecutor executor(test::BigWorker(), command, kWorkerGeneratedProtoDir,
                             5000, &store);

    TaskRequest req = MakeRequest();
    req.workspace = (root / "ws").string();
    executor.Submit(req);
    const auto events = DrainToTerminal(executor, 2000);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, WorkerEventType::kFailed);
    EXPECT_EQ(events.back().error_code, "WORKER_INGEST_FAILED");
    EXPECT_EQ(store.PayloadCount(), 0u);
    std::error_code ec;
    std::filesystem::remove(script, ec);
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_F(ProcessExecutorTest, MaterializesInputsIntoWorkspace) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("spatial_mat_" + std::to_string(std::time(nullptr)) +
                     "_" + std::to_string(std::rand()));
  std::filesystem::create_directories(root);
  {
    auto db = MetadataDb::Create(root / "p.db");
    ArtifactStore store(root / "artifacts", db);
    ArtifactManifest manifest;
    manifest.type = "demo_input";
    manifest.producer.id = "test";
    manifest.producer.version = "1.0.0";
    manifest.mime_type = "text/plain";
    const std::vector<std::uint8_t> seed = {'s', 'e', 'e', 'd'};
    const auto written = store.Put(seed, manifest);

    auto executor = MakeExecutor({}, &store);
    TaskRequest req = MakeRequest();
    req.input_refs = {written.content_hash};
    req.workspace = (root / "ws").string();
    executor->Submit(req);

    const auto events = DrainToTerminal(*executor, 2000);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().type, WorkerEventType::kCompleted);

    // The input arrived in workspace/inputs/<hash> with the exact CAS bytes.
    const auto materialized = std::filesystem::path(req.workspace) / "inputs" /
                              written.content_hash;
    EXPECT_TRUE(std::filesystem::exists(materialized));
    const auto got = spatial::core::fs::ReadFile(materialized);
    EXPECT_EQ(got, seed);

    // The worker consumed the input: its payload embeds "|input:seed".
    bool saw_artifact = false;
    for (const auto& e : events) {
      if (e.type == WorkerEventType::kArtifactProduced) {
        saw_artifact = true;
      }
    }
    EXPECT_TRUE(saw_artifact);
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_F(ProcessExecutorTest, MaterializeMissingInputFailsClosed) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("spatial_mat_" + std::to_string(std::time(nullptr)) +
                     "_" + std::to_string(std::rand()));
  std::filesystem::create_directories(root);
  {
    auto db = MetadataDb::Create(root / "p.db");
    ArtifactStore store(root / "artifacts", db);
    auto executor = MakeExecutor({}, &store);

    TaskRequest req = MakeRequest();
    req.input_refs = {std::string(64, 'd')};  // not in CAS
    req.workspace = (root / "ws").string();
    executor->Submit(req);

    WorkerEvent event;
    ASSERT_TRUE(executor->WaitForEvent(event, 2000));
    EXPECT_EQ(event.type, WorkerEventType::kFailed);
    EXPECT_EQ(event.error_code, "WORKER_INPUT_MATERIALIZE");
    EXPECT_FALSE(event.error_message.empty());
    executor->Shutdown();
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

TEST_F(ProcessExecutorTest, MaterializeWithoutStoreFailsClosed) {
  auto executor = MakeExecutor();  // no store

  TaskRequest req = MakeRequest();
  req.input_refs = {std::string(64, 'a')};
  req.workspace = "temp/job/task";
  executor->Submit(req);

  WorkerEvent event;
  ASSERT_TRUE(executor->WaitForEvent(event, 2000));
  EXPECT_EQ(event.type, WorkerEventType::kFailed);
  EXPECT_EQ(event.error_code, "WORKER_INPUT_MATERIALIZE");
  executor->Shutdown();
}

}  // namespace
}  // namespace spatial::engine
