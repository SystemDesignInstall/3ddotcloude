#include "engine/pipeline/pipeline_compiler.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"
#include "engine/cache/task_cache.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::GenerateUuid;
using spatial::core::Sha256Hex;
using spatial::core::ValidationError;
using spatial::core::fs::TimestampNsNow;
using nlohmann::json;

std::string JoinSorted(const std::vector<ArtifactRef>& refs) {
  std::vector<ArtifactRef> sorted = refs;
  std::sort(sorted.begin(), sorted.end());
  std::string out;
  for (const auto& ref : sorted) {
    out += ref;
    out += '|';
  }
  return out;
}

// Stable placeholder ref for a stage output: identical across runs so the
// DAG type-match rule (task-model §2) holds before real refs are threaded.
std::string PlaceholderRef(const std::string& pipeline_id,
                           const std::string& stage_id) {
  return pipeline_id + "/" + stage_id + "/output";
}

}  // namespace

PipelineCompiler::PipelineCompiler(std::string producer_version,
                                   std::string engine_git_commit)
    : producer_version_(std::move(producer_version)),
      engine_git_commit_(std::move(engine_git_commit)) {}

std::string PipelineCompiler::ConfigHash(const std::string& config_json) {
  return Sha256Hex(config_json);
}

std::string PipelineCompiler::PipelineHash(
    const PipelineDefinition& pipeline, const std::string& config_hash,
    const std::vector<ArtifactRef>& external_inputs) {
  std::string content = pipeline.id;
  content += "|" + pipeline.version;
  content += "|" + pipeline.git_commit;
  content += "|" + config_hash;
  content += "|" + JoinSorted(external_inputs);
  return Sha256Hex(content);
}

ExecutionPlan PipelineCompiler::Compile(
    const PipelineDefinition& pipeline,
    const std::vector<ArtifactRef>& external_inputs,
    const std::string& config_json,
    const ResourceProfile& worker_profile,
    const std::string& implementation) const {
  if (pipeline.stages.empty()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "pipeline '" + pipeline.id + "' has no stages");
  }
  if (external_inputs.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "pipeline '" + pipeline.id + "' requires at least one external input");
  }
  json parsed_config;
  try {
    parsed_config = config_json.empty() ? json::object()
                                        : json::parse(config_json);
  } catch (const json::parse_error&) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "invalid pipeline configuration JSON");
  }

  // Capability binding (ADR-011): every stage must be satisfied by the worker
  // profile the executor advertises, BEFORE any task is created.
  for (const auto& stage : pipeline.stages) {
    if (!HasCapabilities(worker_profile.capabilities, {stage.capability})) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "pipeline '" + pipeline.id + "': no worker advertises capability '" +
              stage.capability + "' (stage '" + stage.id + "')");
    }
  }

  ExecutionPlan plan;
  plan.pipeline_id = pipeline.id;
  plan.pipeline_version = pipeline.version;
  plan.git_commit = pipeline.git_commit;
  plan.config_hash = ConfigHash(config_json);
  plan.external_inputs = external_inputs;
  plan.pipeline_hash =
      PipelineHash(pipeline, plan.config_hash, external_inputs);
  plan.created_at_ns = TimestampNsNow();
  plan.job_id = GenerateUuid();
  plan.graph = std::make_unique<TaskGraph>(plan.job_id);

  std::string previous_placeholder;
  for (std::size_t i = 0; i < pipeline.stages.size(); ++i) {
    const auto& stage = pipeline.stages[i];
    const std::string placeholder =
        PlaceholderRef(pipeline.id, stage.id);

    TaskInstance task;
    task.definition.type = stage.task_type;
    task.definition.inputs.artifact_kinds = stage.input_artifact_kinds;
    task.definition.inputs.allow_external = (i == 0);
    task.definition.outputs.artifact_kinds = stage.output_artifact_kinds;
    task.definition.requirements.cores = 1;

    if (i == 0) {
      task.inputs = external_inputs;  // real CAS refs
    } else {
      task.inputs = {previous_placeholder};
    }
    task.outputs = {placeholder};

    // Effective stage configuration: canonical JSON so identical runs hash
    // identically (AC-8 / ADR-020).
    const json stage_config = {{"pipeline_id", pipeline.id},
                               {"pipeline_version", pipeline.version},
                               {"stage", stage.id},
                               {"config", parsed_config}};
    task.config_json = stage_config.dump();

    task.metadata.deterministic = true;
    task.metadata.created_at_ns = plan.created_at_ns;
    task.metadata.updated_at_ns = plan.created_at_ns;

    const Uuid task_id = plan.graph->AddTask(std::move(task));
    if (i > 0) {
      plan.graph->AddDependency(task_id,
                                plan.stages.back().task_id);
    }

    PlanStage compiled;
    compiled.stage_id = stage.id;
    compiled.capability = stage.capability;
    compiled.implementation = implementation;
    compiled.task_type = stage.task_type;
    compiled.config_hash = Sha256Hex(
        plan.graph->GetTask(task_id).config_json);
    compiled.task_hash = TaskCache::ComputeKey(
        stage.task_type, plan.graph->GetTask(task_id).inputs,
        compiled.config_hash, producer_version_, engine_git_commit_);
    compiled.task_id = task_id;
    plan.stages.push_back(std::move(compiled));

    previous_placeholder = placeholder;
  }
  return plan;
}

}  // namespace spatial::engine
