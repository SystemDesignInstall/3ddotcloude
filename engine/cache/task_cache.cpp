#include "engine/cache/task_cache.h"

#include <algorithm>
#include <utility>

#include "core/utils/fs.h"
#include "core/utils/sha256.h"

namespace spatial::engine {

using spatial::core::Sha256Hex;

TaskCache::TaskCache(ArtifactStore& store, SchedulerStateStore& state,
                     std::string producer_version,
                     std::string engine_git_commit)
    : store_(store),
      state_(state),
      producer_version_(std::move(producer_version)),
      engine_git_commit_(std::move(engine_git_commit)) {}

std::string TaskCache::ComputeKey(std::string_view task_type,
                                  const std::vector<ArtifactRef>& inputs,
                                  std::string_view config_hash,
                                  std::string_view producer_version,
                                  std::string_view engine_git_commit) {
  std::vector<ArtifactRef> sorted = inputs;
  std::sort(sorted.begin(), sorted.end());
  std::string joined;
  for (const auto& ref : sorted) {
    joined.append(ref);
    joined.push_back('\n');
  }
  std::string preimage;
  preimage.append(task_type);
  preimage.push_back('\n');
  preimage.append(joined);
  preimage.append(config_hash);
  preimage.push_back('\n');
  preimage.append(producer_version);
  preimage.push_back('\n');
  preimage.append(engine_git_commit);
  return Sha256Hex(preimage);
}

std::string TaskCache::ConfigHash(const TaskInstance& task) {
  return Sha256Hex(task.config_json);
}

bool TaskCache::IsCacheable(const TaskInstance& task) const noexcept {
  return task.metadata.deterministic &&
         task.metadata.cache == CachePolicy::kCacheable;
}

std::optional<CacheEntryRecord> TaskCache::Lookup(
    const TaskInstance& task) const {
  if (!IsCacheable(task)) {
    return std::nullopt;
  }
  const std::string key =
      ComputeKey(task.definition.type, task.inputs, ConfigHash(task),
                 producer_version_, engine_git_commit_);
  auto entry = state_.FindCacheEntry(key);
  if (!entry) {
    return std::nullopt;
  }
  // A cache hit must still be backed by the payload in CAS (ADR-010: reads
  // re-verify). If the artifact was garbage collected, treat as a miss and
  // drop the stale key.
  if (!store_.HasManifest(entry->artifact_id)) {
    state_.DeleteCacheEntry(key);
    return std::nullopt;
  }
  return entry;
}

std::optional<ArtifactRef> TaskCache::ResolveOutput(
    const CacheEntryRecord& entry) const {
  const auto manifest = store_.ReadManifest(entry.artifact_id);
  if (!manifest) {
    return std::nullopt;
  }
  return manifest->content_hash;
}

void TaskCache::Store(const TaskInstance& task, const Uuid& artifact_id) {  if (!IsCacheable(task)) {
    return;
  }
  CacheEntryRecord entry;
  entry.cache_key =
      ComputeKey(task.definition.type, task.inputs, ConfigHash(task),
                 producer_version_, engine_git_commit_);
  entry.artifact_id = artifact_id;
  entry.task_type = task.definition.type;
  entry.producer_version = producer_version_;
  entry.git_commit = engine_git_commit_;
  entry.config_hash = ConfigHash(task);
  entry.created_at_ns = spatial::core::fs::TimestampNsNow();
  entry.status = "valid";
  state_.UpsertCacheEntry(entry);
}

void TaskCache::StoreOutput(const TaskInstance& task,
                            const ArtifactRef& output_ref) {
  if (!IsCacheable(task)) {
    return;
  }
  const auto row = state_.FindArtifactByHash(output_ref);
  if (!row) {
    return;
  }
  Store(task, row->artifact_id);
}

void TaskCache::Invalidate(const std::string& cache_key) {
  state_.DeleteCacheEntry(cache_key);
}

}  // namespace spatial::engine
