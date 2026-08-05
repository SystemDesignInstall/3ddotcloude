#pragma once

// Persistence records for the 0003_scheduler.sql tables (RFC-0003 §5.12).
// These mirror the workers and cache_entries rows; the tasks and task_runs
// rows are (de)serialized through the Task model and ExecutionRecord.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/engine_common.h"

namespace spatial::engine {

struct WorkerRecord {
  Uuid worker_id{};
  std::string name;
  std::vector<std::string> capabilities;  // worker-capabilities.schema.json
  std::string resource_profile_json;
  int protocol_version = 1;
  int max_concurrency = 1;
  std::optional<std::int64_t> last_heartbeat_ns;
  std::string status = "idle";
};

struct CacheEntryRecord {
  std::string cache_key;       // ADR-020 composite key (primary key)
  Uuid artifact_id{};
  std::string task_type;
  std::string producer_version;
  std::string git_commit;
  std::string config_hash;
  std::int64_t created_at_ns = 0;
  std::string status = "valid";
};

}  // namespace spatial::engine
