#include "engine/scheduler/scheduler_state_store.h"

#include <cstring>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/errors/project_error.h"
#include "core/utils/sha256.h"
#include "engine/task/task_serialization.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::ParseUuid;
using spatial::core::SchedulerError;
using spatial::core::Sha256Hex;
using nlohmann::json;

std::string ConfigHash(const TaskInstance& task) {
  return Sha256Hex(task.config_json);
}

// Blob helpers: engine ids are 16-byte blobs (RFC-4122).
std::string UuidToBlobBytes(const Uuid& uuid) {
  return std::string(reinterpret_cast<const char*>(uuid.data()), uuid.size());
}

Uuid BlobBytesToUuid(const void* blob, int size) {
  Uuid out{};
  if (blob != nullptr && size == static_cast<int>(out.size())) {
    std::memcpy(out.data(), blob, out.size());
  }
  return out;
}

void BindUuid(sqlite3_stmt* stmt, int index, const Uuid& uuid) {
  sqlite3_bind_blob(stmt, index, uuid.data(), static_cast<int>(uuid.size()),
                    SQLITE_TRANSIENT);
}

[[noreturn]] void ThrowSql(const char* what, sqlite3* db) {
  throw SchedulerError(ErrorCode::kSchedPersistence,
                       std::string(what) + ": " + sqlite3_errmsg(db), {},
                       false,
                       "The scheduler state in project.db is corrupt.");
}

}  // namespace

SchedulerStateStore::SchedulerStateStore(MetadataDb& db) : db_(db) {}

void SchedulerStateStore::SaveGraph(const TaskGraph& graph) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr,
                   nullptr) != SQLITE_OK) {
    ThrowSql("cannot begin SaveGraph transaction", db);
  }
  try {
    const char* insert_task =
        "INSERT INTO tasks (task_id, job_id, task_type, spec_json, config_hash,"
        " cache_policy, deterministic, cancellation_policy, status,"
        " retry_policy_json, created_at_ns, updated_at_ns)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
    if (sqlite3_prepare_v2(db, insert_task, -1, &stmt, nullptr) != SQLITE_OK) {
      ThrowSql("cannot prepare insert task", db);
    }
    for (const auto& id : graph.Order()) {
      const auto& task = graph.GetTask(id);
      BindUuid(stmt, 1, task.id);
      BindUuid(stmt, 2, graph.job_id());
      sqlite3_bind_text(stmt, 3, task.definition.type.c_str(), -1,
                        SQLITE_TRANSIENT);
      const std::string spec_json = TaskToJson(task);
      sqlite3_bind_text(stmt, 4, spec_json.c_str(), -1, SQLITE_TRANSIENT);
      const std::string config_hash = ConfigHash(task);
      sqlite3_bind_text(stmt, 5, config_hash.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 6,
                        task.metadata.cache == CachePolicy::kNever ? "never"
                                                                   : "cacheable",
                        -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 7, task.metadata.deterministic ? 1 : 0);
      sqlite3_bind_text(
          stmt, 8,
          task.metadata.cancellation == CancellationPolicy::kBestEffort
              ? "best_effort"
              : "cooperative",
          -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 9, TaskStatusName(task.state), -1,
                        SQLITE_TRANSIENT);
      const json retry_json = {{"max_attempts", task.metadata.retry.max_attempts},
                               {"base_ns", task.metadata.retry.base_ns},
                               {"multiplier", task.metadata.retry.multiplier},
                               {"max_ns", task.metadata.retry.max_ns}};
      const std::string retry = retry_json.dump();
      sqlite3_bind_text(stmt, 10, retry.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt, 11, task.metadata.created_at_ns);
      sqlite3_bind_int64(stmt, 12, task.metadata.updated_at_ns);
      if (sqlite3_step(stmt) != SQLITE_DONE) {
        ThrowSql("cannot insert task row", db);
      }
      sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;

    const char* insert_dep =
        "INSERT INTO task_dependencies (task_id, dependency_id)"
        " VALUES (?, ?)";
    if (sqlite3_prepare_v2(db, insert_dep, -1, &stmt, nullptr) != SQLITE_OK) {
      ThrowSql("cannot prepare insert dependency", db);
    }
    for (const auto& id : graph.Order()) {
      for (const auto& dep : graph.DependenciesOf(id)) {
        BindUuid(stmt, 1, id);
        BindUuid(stmt, 2, dep);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
          ThrowSql("cannot insert dependency row", db);
        }
        sqlite3_reset(stmt);
      }
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;

    if (sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      ThrowSql("cannot commit SaveGraph transaction", db);
    }
  } catch (...) {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    throw;
  }
}

void SchedulerStateStore::UpsertTask(const Uuid& job_id,
                                     const TaskInstance& task) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  const char* sql =
      "INSERT INTO tasks (task_id, job_id, task_type, spec_json, config_hash,"
      " cache_policy, deterministic, cancellation_policy, status,"
      " retry_policy_json, created_at_ns, updated_at_ns)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
      " ON CONFLICT(task_id) DO UPDATE SET"
      " spec_json=excluded.spec_json, config_hash=excluded.config_hash,"
      " cache_policy=excluded.cache_policy,"
      " deterministic=excluded.deterministic,"
      " cancellation_policy=excluded.cancellation_policy,"
      " status=excluded.status, retry_policy_json=excluded.retry_policy_json,"
      " updated_at_ns=excluded.updated_at_ns";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare upsert task", db);
  }
  BindUuid(stmt, 1, task.id);
  BindUuid(stmt, 2, job_id);
  sqlite3_bind_text(stmt, 3, task.definition.type.c_str(), -1, SQLITE_TRANSIENT);
  const std::string spec_json = TaskToJson(task);
  sqlite3_bind_text(stmt, 4, spec_json.c_str(), -1, SQLITE_TRANSIENT);
  const std::string config_hash = ConfigHash(task);
  sqlite3_bind_text(stmt, 5, config_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6,
                    task.metadata.cache == CachePolicy::kNever ? "never"
                                                               : "cacheable",
                    -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 7, task.metadata.deterministic ? 1 : 0);
  sqlite3_bind_text(
      stmt, 8,
      task.metadata.cancellation == CancellationPolicy::kBestEffort
          ? "best_effort"
          : "cooperative",
      -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, TaskStatusName(task.state), -1, SQLITE_TRANSIENT);
  const json retry_json = {{"max_attempts", task.metadata.retry.max_attempts},
                           {"base_ns", task.metadata.retry.base_ns},
                           {"multiplier", task.metadata.retry.multiplier},
                           {"max_ns", task.metadata.retry.max_ns}};
  const std::string retry = retry_json.dump();
  sqlite3_bind_text(stmt, 10, retry.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 11, task.metadata.created_at_ns);
  sqlite3_bind_int64(stmt, 12, task.metadata.updated_at_ns);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot upsert task row", db);
  }
  sqlite3_finalize(stmt);
}

void SchedulerStateStore::RecordRun(const ExecutionRecord& record) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  const char* sql =
      "INSERT INTO task_runs (run_id, task_id, attempt, worker_id,"
      " started_at_ns, ended_at_ns, terminal_state, error_json,"
      " input_refs_json, output_refs_json, environment_json, hardware_json)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare insert run", db);
  }
  BindUuid(stmt, 1, record.id);
  BindUuid(stmt, 2, record.task_id);
  sqlite3_bind_int(stmt, 3, record.attempt);
  if (record.worker_id) {
    BindUuid(stmt, 4, *record.worker_id);
  } else {
    sqlite3_bind_null(stmt, 4);
  }
  if (record.started_at_ns != 0) {
    sqlite3_bind_int64(stmt, 5, record.started_at_ns);
  } else {
    sqlite3_bind_null(stmt, 5);
  }
  if (record.ended_at_ns != 0) {
    sqlite3_bind_int64(stmt, 6, record.ended_at_ns);
  } else {
    sqlite3_bind_null(stmt, 6);
  }
  sqlite3_bind_text(stmt, 7, TaskStatusName(record.terminal_state), -1,
                    SQLITE_TRANSIENT);
  const json inputs(record.inputs);
  const json outputs(record.outputs);
  const std::string inputs_json = inputs.dump();
  const std::string outputs_json = outputs.dump();
  const std::string environment_json = ToJson(record.environment);
  const std::string hardware_json = ToJson(record.hardware);
  std::string error_json = "null";
  if (record.error) {
    error_json =
        json{{"code", record.error->code},
             {"message", record.error->message},
             {"recoverable", record.error->recoverable}}
            .dump();
  }
  sqlite3_bind_text(stmt, 8, error_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 9, inputs_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, outputs_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11, environment_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 12, hardware_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot insert run row", db);
  }
  sqlite3_finalize(stmt);
}

void SchedulerStateStore::UpsertWorker(const WorkerRecord& worker) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  const char* sql =
      "INSERT OR REPLACE INTO workers (worker_id, name, capabilities_json,"
      " resource_profile_json, protocol_version, max_concurrency,"
      " last_heartbeat_ns, status)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare upsert worker", db);
  }
  BindUuid(stmt, 1, worker.worker_id);
  sqlite3_bind_text(stmt, 2, worker.name.c_str(), -1, SQLITE_TRANSIENT);
  const json caps(worker.capabilities);
  const std::string caps_json = caps.dump();
  sqlite3_bind_text(stmt, 3, caps_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, worker.resource_profile_json.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 5, worker.protocol_version);
  sqlite3_bind_int(stmt, 6, worker.max_concurrency);
  if (worker.last_heartbeat_ns) {
    sqlite3_bind_int64(stmt, 7, *worker.last_heartbeat_ns);
  } else {
    sqlite3_bind_null(stmt, 7);
  }
  sqlite3_bind_text(stmt, 8, worker.status.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot upsert worker row", db);
  }
  sqlite3_finalize(stmt);
}

void SchedulerStateStore::UpsertCacheEntry(const CacheEntryRecord& entry) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  const char* sql =
      "INSERT OR REPLACE INTO cache_entries (cache_key, artifact_id, task_type,"
      " producer_version, git_commit, config_hash, created_at_ns, status)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare upsert cache entry", db);
  }
  sqlite3_bind_text(stmt, 1, entry.cache_key.c_str(), -1, SQLITE_TRANSIENT);
  BindUuid(stmt, 2, entry.artifact_id);
  sqlite3_bind_text(stmt, 3, entry.task_type.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, entry.producer_version.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, entry.git_commit.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, entry.config_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, entry.created_at_ns);
  sqlite3_bind_text(stmt, 8, entry.status.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot upsert cache entry", db);
  }
  sqlite3_finalize(stmt);
}

std::vector<TaskInstance> SchedulerStateStore::LoadTasks(
    const Uuid& job_id) const {
  std::vector<TaskInstance> out;
  sqlite3* db = db_.db();
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT spec_json FROM tasks WHERE job_id = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare load tasks", db);
  }
  BindUuid(stmt, 1, job_id);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (const auto* t = sqlite3_column_text(stmt, 0)) {
      out.push_back(TaskFromJson(reinterpret_cast<const char*>(t)));
    }
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<std::pair<Uuid, Uuid>> SchedulerStateStore::LoadDependencies(
    const Uuid& job_id) const {
  std::vector<std::pair<Uuid, Uuid>> out;
  sqlite3* db = db_.db();
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT d.task_id, d.dependency_id FROM task_dependencies d"
      " JOIN tasks t ON t.task_id = d.task_id"
      " WHERE t.job_id = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare load dependencies", db);
  }
  BindUuid(stmt, 1, job_id);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const Uuid task_id =
        BlobBytesToUuid(sqlite3_column_blob(stmt, 0),
                        sqlite3_column_bytes(stmt, 0));
    const Uuid dep_id = BlobBytesToUuid(sqlite3_column_blob(stmt, 1),
                                        sqlite3_column_bytes(stmt, 1));
    out.emplace_back(task_id, dep_id);
  }
  sqlite3_finalize(stmt);
  return out;
}

std::vector<ExecutionRecord> SchedulerStateStore::LoadRuns(
    const Uuid& task_id) const {
  std::vector<ExecutionRecord> out;
  sqlite3* db = db_.db();
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT run_id, task_id, attempt, worker_id, started_at_ns, ended_at_ns,"
      " terminal_state, error_json, input_refs_json, output_refs_json,"
      " environment_json, hardware_json FROM task_runs WHERE task_id = ?"
      " ORDER BY attempt";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare load runs", db);
  }
  BindUuid(stmt, 1, task_id);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    json j;
    j["id"] = FormatUuid(
        BlobBytesToUuid(sqlite3_column_blob(stmt, 0),
                        sqlite3_column_bytes(stmt, 0)));
    j["task_id"] = FormatUuid(
        BlobBytesToUuid(sqlite3_column_blob(stmt, 1),
                        sqlite3_column_bytes(stmt, 1)));
    j["attempt"] = sqlite3_column_int(stmt, 2);
    const void* worker_blob = sqlite3_column_blob(stmt, 3);
    if (worker_blob != nullptr &&
        sqlite3_column_bytes(stmt, 3) == 16) {
      j["worker_id"] =
          FormatUuid(BlobBytesToUuid(worker_blob, 16));
    } else {
      j["worker_id"] = nullptr;
    }
    const auto col_started = sqlite3_column_int64(stmt, 4);
    j["started_at_ns"] = col_started;
    j["ended_at_ns"] = sqlite3_column_int64(stmt, 5);
    j["terminal_state"] = sqlite3_column_type(stmt, 6) == SQLITE_NULL
                              ? "failed"
                              : reinterpret_cast<const char*>(
                                    sqlite3_column_text(stmt, 6));
    j["error"] = json::parse(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
    j["inputs"] = json::parse(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)));
    j["outputs"] = json::parse(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)));
    j["environment"] = json::parse(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10)));
    j["hardware"] = json::parse(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11)));
    out.push_back(ExecutionRecordFromJson(j.dump()));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::optional<TaskInstance> SchedulerStateStore::FindTask(
    const Uuid& task_id) const {
  sqlite3* db = db_.db();
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT spec_json FROM tasks WHERE task_id = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare find task", db);
  }
  BindUuid(stmt, 1, task_id);
  std::optional<TaskInstance> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    if (const auto* t = sqlite3_column_text(stmt, 0)) {
      out = TaskFromJson(reinterpret_cast<const char*>(t));
    }
  }
  sqlite3_finalize(stmt);
  return out;
}

std::optional<CacheEntryRecord> SchedulerStateStore::FindCacheEntry(
    const std::string& cache_key) const {  sqlite3* db = db_.db();
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT cache_key, artifact_id, task_type, producer_version, git_commit,"
      " config_hash, created_at_ns, status FROM cache_entries"
      " WHERE cache_key = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare find cache entry", db);
  }
  sqlite3_bind_text(stmt, 1, cache_key.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<CacheEntryRecord> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    CacheEntryRecord entry;
    entry.cache_key =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    entry.artifact_id =
        BlobBytesToUuid(sqlite3_column_blob(stmt, 1),
                        sqlite3_column_bytes(stmt, 1));
    entry.task_type =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    entry.producer_version =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    entry.git_commit =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    entry.config_hash =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    entry.created_at_ns = sqlite3_column_int64(stmt, 6);
    entry.status =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    out = entry;
  }
  sqlite3_finalize(stmt);
  return out;
}

void SchedulerStateStore::DeleteCacheEntry(const std::string& cache_key) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "DELETE FROM cache_entries WHERE cache_key = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare delete cache entry", db);
  }
  sqlite3_bind_text(stmt, 1, cache_key.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot delete cache entry", db);
  }
  sqlite3_finalize(stmt);
}

}  // namespace spatial::engine
