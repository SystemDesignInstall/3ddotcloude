#pragma once

// ExecutionManifest - the pipeline-level execution document (RFC-0003 §5.1,
// migration 0004). It is the golden source for Resume / Audit /
// Reproducibility and the QualityReport link (RFC-0005). Owned by
// engine/pipeline and persisted through the single MetadataDb connection; the
// scheduler stays agnostic and never writes manifest rows.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/storage/metadata_db.h"
#include "engine/engine_common.h"
#include "engine/pipeline/execution_plan.h"
#include "engine/task/task_types.h"

namespace spatial::engine {

struct ExecutionManifestStage {
  int sequence = 0;
  std::string stage_id;
  std::string capability;
  std::string implementation;
  std::string task_hash;              // ADR-020 cache key of the stage task
  std::string status;                 // TaskStatusName
  bool cache_hit = false;
  Uuid task_id{};
  std::vector<ArtifactRef> output_refs;  // produced CAS content hashes
  std::int64_t started_at_ns = 0;
  std::int64_t finished_at_ns = 0;
};

struct ExecutionManifest {
  Uuid manifest_id{};
  std::string pipeline_id;
  std::string pipeline_version;
  std::string pipeline_hash;
  std::string config_hash;
  std::string git_commit;
  std::string status;                 // running|succeeded|failed|cancelled
  std::vector<ArtifactRef> external_inputs;
  std::optional<Uuid> quality_report_id;  // RFC-0005: validate-stage artifact
  std::int64_t created_at_ns = 0;
  std::int64_t finished_at_ns = 0;
  std::vector<ExecutionManifestStage> stages;
};

// Renders the manifest as canonical JSON (CLI / API surface).
std::string ToJson(const ExecutionManifest& manifest);

// Persistence for the manifest tables (migration 0004).
class ExecutionManifestStore {
 public:
  // `db` must outlive this store (single SQLite writer, ADR-020).
  explicit ExecutionManifestStore(spatial::core::MetadataDb& db);

  // Inserts the header row (status 'running').
  void Begin(const ExecutionPlan& plan);

  // Inserts one stage row (status 'pending').
  void InsertStage(const Uuid& manifest_id, const PlanStage& stage,
                   int sequence);

  // Updates one stage row with its terminal state and produced refs.
  void UpdateStage(const Uuid& manifest_id, int sequence, TaskStatus status,
                   bool cache_hit, std::vector<ArtifactRef> output_refs,
                   std::int64_t started_at_ns, std::int64_t finished_at_ns);

  // Marks the whole pipeline terminal.
  void Finish(const Uuid& manifest_id, TaskStatus pipeline_status);

  // Links the quality report artifact produced by the validate stage
  // (RFC-0005): quality_report_id = artifact_uuid.
  void SetQualityReportId(const Uuid& manifest_id, const Uuid& artifact_uuid);

  std::optional<ExecutionManifest> Load(const Uuid& manifest_id) const;
  std::vector<Uuid> ListIds() const;

 private:
  spatial::core::MetadataDb& db_;
};

}  // namespace spatial::engine
