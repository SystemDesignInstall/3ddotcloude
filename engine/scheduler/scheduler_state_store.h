#pragma once

// SchedulerStateStore - persistence of run state in project.db
// (RFC-0003 §5.12, scheduler-design §6). Owns the five engine tables of
// migration 0003 through the single MetadataDb connection (ADR-020: exactly
// one SQLite writer, WAL, metadata only). Every state transition is recorded
// transactionally so an interrupted run resumes from the last persisted
// state.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/storage/metadata_db.h"
#include "engine/execution/execution_record.h"
#include "engine/scheduler/scheduler_types.h"
#include "engine/task/task_graph.h"
#include "engine/task/task_instance.h"

namespace spatial::engine {

using spatial::core::MetadataDb;

class SchedulerStateStore {
 public:
  // `db` must outlive this store (engine.md §2: core/storage owns the
  // connection).
  explicit SchedulerStateStore(MetadataDb& db);

  // Persists a whole graph (tasks + task_dependencies) in one transaction.
  void SaveGraph(const TaskGraph& graph);

  // Records a state transition: upserts the task row (status + spec +
  // metadata) atomically.
  void UpsertTask(const Uuid& job_id, const TaskInstance& task);

  // Appends one run record (task_runs row).
  void RecordRun(const ExecutionRecord& record);

  void UpsertWorker(const WorkerRecord& worker);
  void UpsertCacheEntry(const CacheEntryRecord& entry);

  // Resume support (scheduler-design §6).
  std::vector<TaskInstance> LoadTasks(const Uuid& job_id) const;
  std::vector<std::pair<Uuid, Uuid>> LoadDependencies(const Uuid& job_id) const;
  std::vector<ExecutionRecord> LoadRuns(const Uuid& task_id) const;
  std::optional<TaskInstance> FindTask(const Uuid& task_id) const;
  std::optional<CacheEntryRecord> FindCacheEntry(
      const std::string& cache_key) const;
  void DeleteCacheEntry(const std::string& cache_key);

  // Passthrough to the artifact index (core/storage). The task cache uses it
  // to resolve a produced content hash back to its artifact id when
  // registering a cache entry.
  std::optional<spatial::core::ArtifactIndexRow> FindArtifactByHash(
      const std::string& content_hash) const {
    return db_.FindArtifactByHash(content_hash);
  }

 private:
  MetadataDb& db_;
};

}  // namespace spatial::engine
