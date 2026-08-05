#pragma once

// Task cache (RFC-0003 §5.11, ADR-020, scheduler-design §7). The cache key is
// the full composite SHA-256 over task type, sorted input CAS content hashes,
// effective config hash, producer version, and the engine git commit. Only
// deterministic + cacheable tasks are served from the cache; cache_policy =
// never bypasses it entirely. Artifact bytes live in CAS; the key -> ref
// mapping lives in project.db (cache_entries, migration 0003).

#include <optional>
#include <string>
#include <string_view>

#include "core/artifacts/artifact_store.h"
#include "engine/engine_common.h"
#include "engine/scheduler/scheduler_types.h"
#include "engine/scheduler/scheduler_state_store.h"
#include "engine/task/task_instance.h"

namespace spatial::engine {

using spatial::core::ArtifactStore;

class TaskCache {
 public:
  // `store` backs the artifact bytes; `state` owns cache_entries. Both must
  // outlive this cache. `producer_version` and `engine_git_commit` are fixed
  // for the engine build (embedded at configure time).
  TaskCache(ArtifactStore& store, SchedulerStateStore& state,
            std::string producer_version, std::string engine_git_commit);

  // ADR-020 composite key:
  //   SHA-256(task_type || sorted(input content hashes) || config_hash
  //           || producer_version || engine_git_commit)
  // Inputs are sorted here, so caller order does not matter.
  static std::string ComputeKey(std::string_view task_type,
                                const std::vector<ArtifactRef>& inputs,
                                std::string_view config_hash,
                                std::string_view producer_version,
                                std::string_view engine_git_commit);

  // The effective config hash for a task (Sha256Hex of config_json).
  static std::string ConfigHash(const TaskInstance& task);

  // Cache hit for a deterministic + cacheable task, or nullopt. The cached
  // entry points at the artifact id in the store; callers resolve the
  // content hash through ArtifactStore::ReadManifest.
  std::optional<CacheEntryRecord> Lookup(const TaskInstance& task) const;

  // Resolves the cached artifact's CAS content hash from its manifest.
  // Returns nullopt when the manifest is gone (stale entry).
  std::optional<ArtifactRef> ResolveOutput(
      const CacheEntryRecord& entry) const;

  // Stores a cacheable output (key -> produced artifact).
  void Store(const TaskInstance& task, const Uuid& artifact_id);

  // Registers the cache entry for a produced output ref. The ref is resolved
  // to its artifact id through the artifact index; outputs that were never
  // registered in CAS are not cached (the artifact store owns the bytes).
  void StoreOutput(const TaskInstance& task, const ArtifactRef& output_ref);

  // Drops a key from the cache (e.g. the payload was garbage collected).
  void Invalidate(const std::string& cache_key);

 private:
  bool IsCacheable(const TaskInstance& task) const noexcept;

  ArtifactStore& store_;
  SchedulerStateStore& state_;
  std::string producer_version_;
  std::string engine_git_commit_;
};

}  // namespace spatial::engine
