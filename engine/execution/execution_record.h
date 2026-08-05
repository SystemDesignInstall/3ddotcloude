#pragma once

// ExecutionRecord - per-run provenance (RFC-0003 §5.10), persisted in
// task_runs (0003_scheduler.sql). This is the substrate for reproducibility
// (Constitution Principle 6), audit, comparison (ADR-029), and benchmark
// evidence.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/engine_common.h"
#include "engine/task/task_types.h"

namespace spatial::engine {

struct SoftwareEnvironment {
  std::string engine_version;
  std::string git_commit;
  std::string protocol_version;
};

struct HardwareInfo {
  std::string os;      // "windows" / "linux"
  std::string arch;    // "x86_64"
  std::string cpu;     // descriptive
  std::int64_t ram_bytes = 0;
  std::string gpu;     // empty when none
};

// ADR-014 structured error as recorded in task_runs.error_json.
struct ErrorRecord {
  std::string code;          // stable string, e.g. "WORKER_CRASHED"
  std::string message;
  bool recoverable = false;
};

struct ExecutionRecord {
  Uuid id{};                       // ExecutionId (task_runs.run_id)
  Uuid task_id{};
  int attempt = 1;
  std::optional<Uuid> worker_id;
  std::vector<ArtifactRef> inputs;    // CAS content hashes
  std::vector<ArtifactRef> outputs;   // CAS content hashes
  SoftwareEnvironment environment;
  HardwareInfo hardware;
  std::int64_t started_at_ns = 0;
  std::int64_t ended_at_ns = 0;
  TaskStatus terminal_state = TaskStatus::kFailed;
  std::optional<ErrorRecord> error;
};

// JSON (de)serialization for the task_runs JSON columns.
std::string ToJson(const ExecutionRecord& record);
ExecutionRecord ExecutionRecordFromJson(const std::string& json);

std::string ToJson(const HardwareInfo& hardware);
std::string ToJson(const SoftwareEnvironment& environment);

}  // namespace spatial::engine
