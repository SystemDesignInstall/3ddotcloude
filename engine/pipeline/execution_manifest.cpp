#include "engine/pipeline/execution_manifest.h"

#include <cstring>
#include <utility>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::SchedulerError;
using nlohmann::json;

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
                       "The execution manifest in project.db is corrupt.");
}

std::string JoinJson(const std::vector<ArtifactRef>& refs) {
  return json(refs).dump();
}

std::vector<ArtifactRef> ParseRefs(sqlite3_stmt* stmt, int column) {
  std::vector<ArtifactRef> out;
  if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
    return out;
  }
  const auto* text = sqlite3_column_text(stmt, column);
  if (text == nullptr) {
    return out;
  }
  const auto parsed =
      json::parse(reinterpret_cast<const char*>(text), nullptr,
                  /*allow_exceptions=*/false);
  if (parsed.is_array()) {
    for (const auto& item : parsed) {
      if (item.is_string()) {
        out.push_back(item.get<std::string>());
      }
    }
  }
  return out;
}

}  // namespace

ExecutionManifestStore::ExecutionManifestStore(spatial::core::MetadataDb& db)
    : db_(db) {}

void ExecutionManifestStore::Begin(const ExecutionPlan& plan) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO execution_manifests (manifest_id, pipeline_id,"
      " pipeline_version, pipeline_hash, config_hash, git_commit, status,"
      " external_inputs_json, created_at_ns)"
      " VALUES (?, ?, ?, ?, ?, ?, 'running', ?, ?)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare begin manifest", db);
  }
  BindUuid(stmt, 1, plan.job_id);
  sqlite3_bind_text(stmt, 2, plan.pipeline_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, plan.pipeline_version.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, plan.pipeline_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, plan.config_hash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, plan.git_commit.c_str(), -1, SQLITE_TRANSIENT);
  const std::string inputs = JoinJson(plan.external_inputs);
  sqlite3_bind_text(stmt, 7, inputs.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 8, plan.created_at_ns);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot insert manifest header", db);
  }
  sqlite3_finalize(stmt);
}

void ExecutionManifestStore::InsertStage(const Uuid& manifest_id,
                                         const PlanStage& stage,
                                         int sequence) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO execution_manifest_stages (manifest_id, sequence, stage_id,"
      " capability, implementation, task_hash, status, task_id)"
      " VALUES (?, ?, ?, ?, ?, ?, 'pending', ?)";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare insert stage", db);
  }
  BindUuid(stmt, 1, manifest_id);
  sqlite3_bind_int(stmt, 2, sequence);
  sqlite3_bind_text(stmt, 3, stage.stage_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, stage.capability.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, stage.implementation.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, stage.task_hash.c_str(), -1, SQLITE_TRANSIENT);
  BindUuid(stmt, 7, stage.task_id);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot insert manifest stage", db);
  }
  sqlite3_finalize(stmt);
}

void ExecutionManifestStore::UpdateStage(const Uuid& manifest_id, int sequence,
                                         TaskStatus status, bool cache_hit,
                                         std::vector<ArtifactRef> output_refs,
                                         std::int64_t started_at_ns,
                                         std::int64_t finished_at_ns) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE execution_manifest_stages SET status = ?, cache_hit = ?,"
      " output_refs_json = ?, started_at_ns = ?, finished_at_ns = ?"
      " WHERE manifest_id = ? AND sequence = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare update stage", db);
  }
  sqlite3_bind_text(stmt, 1, TaskStatusName(status), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, cache_hit ? 1 : 0);
  const std::string outputs = JoinJson(output_refs);
  sqlite3_bind_text(stmt, 3, outputs.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, started_at_ns);
  sqlite3_bind_int64(stmt, 5, finished_at_ns);
  BindUuid(stmt, 6, manifest_id);
  sqlite3_bind_int(stmt, 7, sequence);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot update manifest stage", db);
  }
  sqlite3_finalize(stmt);
}

void ExecutionManifestStore::Finish(const Uuid& manifest_id,
                                    TaskStatus pipeline_status) {
  sqlite3* db = db_.db();
  if (db == nullptr) {
    throw SchedulerError(ErrorCode::kSchedPersistence,
                         "metadata database is not open");
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE execution_manifests SET status = ?, finished_at_ns = ?"
      " WHERE manifest_id = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare finish manifest", db);
  }
  sqlite3_bind_text(stmt, 1, TaskStatusName(pipeline_status), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, spatial::core::fs::TimestampNsNow());
  BindUuid(stmt, 3, manifest_id);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    ThrowSql("cannot finish manifest", db);
  }
  sqlite3_finalize(stmt);
}

std::optional<ExecutionManifest> ExecutionManifestStore::Load(
    const Uuid& manifest_id) const {
  sqlite3* db = db_.db();
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT pipeline_id, pipeline_version, pipeline_hash, config_hash,"
      " git_commit, status, external_inputs_json, quality_report_id,"
      " created_at_ns, finished_at_ns FROM execution_manifests"
      " WHERE manifest_id = ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare load manifest", db);
  }
  BindUuid(stmt, 1, manifest_id);
  std::optional<ExecutionManifest> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    ExecutionManifest manifest;
    manifest.manifest_id = manifest_id;
    if (const auto* t = sqlite3_column_text(stmt, 0)) {
      manifest.pipeline_id = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 1)) {
      manifest.pipeline_version = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 2)) {
      manifest.pipeline_hash = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 3)) {
      manifest.config_hash = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 4)) {
      manifest.git_commit = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(stmt, 5)) {
      manifest.status = reinterpret_cast<const char*>(t);
    }
    manifest.external_inputs = ParseRefs(stmt, 6);
    if (sqlite3_column_type(stmt, 7) == SQLITE_BLOB &&
        sqlite3_column_bytes(stmt, 7) == 16) {
      manifest.quality_report_id =
          BlobBytesToUuid(sqlite3_column_blob(stmt, 7),
                          sqlite3_column_bytes(stmt, 7));
    }
    manifest.created_at_ns = sqlite3_column_int64(stmt, 8);
    if (sqlite3_column_type(stmt, 9) == SQLITE_NULL) {
      manifest.finished_at_ns = 0;
    } else {
      manifest.finished_at_ns = sqlite3_column_int64(stmt, 9);
    }
    out = std::move(manifest);
  }
  sqlite3_finalize(stmt);
  if (!out) {
    return out;
  }

  // Stages ordered by sequence.
  sqlite3_stmt* s2 = nullptr;
  const char* stages_sql =
      "SELECT sequence, stage_id, capability, implementation, task_hash,"
      " status, cache_hit, task_id, output_refs_json, started_at_ns,"
      " finished_at_ns FROM execution_manifest_stages WHERE manifest_id = ?"
      " ORDER BY sequence";
  if (sqlite3_prepare_v2(db, stages_sql, -1, &s2, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare load stages", db);
  }
  BindUuid(s2, 1, manifest_id);
  while (sqlite3_step(s2) == SQLITE_ROW) {
    ExecutionManifestStage stage;
    stage.sequence = sqlite3_column_int(s2, 0);
    if (const auto* t = sqlite3_column_text(s2, 1)) {
      stage.stage_id = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(s2, 2)) {
      stage.capability = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(s2, 3)) {
      stage.implementation = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(s2, 4)) {
      stage.task_hash = reinterpret_cast<const char*>(t);
    }
    if (const auto* t = sqlite3_column_text(s2, 5)) {
      stage.status = reinterpret_cast<const char*>(t);
    }
    stage.cache_hit = sqlite3_column_int(s2, 6) != 0;
    stage.task_id = BlobBytesToUuid(sqlite3_column_blob(s2, 7),
                                    sqlite3_column_bytes(s2, 7));
    stage.output_refs = ParseRefs(s2, 8);
    if (sqlite3_column_type(s2, 9) == SQLITE_NULL) {
      stage.started_at_ns = 0;
    } else {
      stage.started_at_ns = sqlite3_column_int64(s2, 9);
    }
    if (sqlite3_column_type(s2, 10) == SQLITE_NULL) {
      stage.finished_at_ns = 0;
    } else {
      stage.finished_at_ns = sqlite3_column_int64(s2, 10);
    }
    out->stages.push_back(std::move(stage));
  }
  sqlite3_finalize(s2);
  return out;
}

std::vector<Uuid> ExecutionManifestStore::ListIds() const {
  std::vector<Uuid> out;
  sqlite3* db = db_.db();
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT manifest_id FROM execution_manifests ORDER BY created_at_ns DESC";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    ThrowSql("cannot prepare list manifests", db);
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(BlobBytesToUuid(sqlite3_column_blob(stmt, 0),
                                  sqlite3_column_bytes(stmt, 0)));
  }
  sqlite3_finalize(stmt);
  return out;
}

std::string ToJson(const ExecutionManifest& manifest) {
  json stages = json::array();
  for (const auto& stage : manifest.stages) {
    stages.push_back({{"sequence", stage.sequence},
                      {"stage_id", stage.stage_id},
                      {"capability", stage.capability},
                      {"implementation", stage.implementation},
                      {"task_hash", stage.task_hash},
                      {"status", stage.status},
                      {"cache_hit", stage.cache_hit},
                      {"task_id", FormatUuid(stage.task_id)},
                      {"output_refs", stage.output_refs}});
  }
  json j = {{"manifest_id", FormatUuid(manifest.manifest_id)},
            {"pipeline",
             {{"id", manifest.pipeline_id},
              {"version", manifest.pipeline_version}}},
            {"pipeline_hash", manifest.pipeline_hash},
            {"config_hash", manifest.config_hash},
            {"git_commit", manifest.git_commit},
            {"status", manifest.status},
            {"external_inputs", manifest.external_inputs},
            {"quality_report_id",
             manifest.quality_report_id
                 ? json(FormatUuid(*manifest.quality_report_id))
                 : json(nullptr)},
            {"created_at_ns", manifest.created_at_ns},
            {"finished_at_ns", manifest.finished_at_ns},
            {"stages", stages}};
  return j.dump(2);
}

}  // namespace spatial::engine
