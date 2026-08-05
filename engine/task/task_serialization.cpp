#include "engine/task/task_serialization.h"

#include <string>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/uuid.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::ParseUuid;
using spatial::core::ValidationError;
using nlohmann::json;

ResourceSpec ResourceSpecFromJson(const json& j) {
  ResourceSpec spec;
  spec.cores = j.value("cores", 1);
  spec.ram_bytes = j.value("ram_bytes", std::int64_t{0});
  spec.gpus = j.value("gpus", 0);
  spec.gpu_mem_bytes = j.value("gpu_mem_bytes", std::int64_t{0});
  spec.temp_disk_bytes = j.value("temp_disk_bytes", std::int64_t{0});
  return spec;
}

json ResourceSpecToJson(const ResourceSpec& spec) {
  json j = {{"cores", spec.cores},
            {"ram_bytes", spec.ram_bytes},
            {"gpus", spec.gpus},
            {"gpu_mem_bytes", spec.gpu_mem_bytes},
            {"temp_disk_bytes", spec.temp_disk_bytes}};
  return j;
}

RetryPolicy RetryPolicyFromJson(const json& j) {
  RetryPolicy policy;
  policy.max_attempts = j.value("max_attempts", 2);
  policy.base_ns = j.value("base_ns", std::int64_t{1000000000});
  policy.multiplier = j.value("multiplier", 2.0);
  policy.max_ns = j.value("max_ns", std::int64_t{60000000000});
  return policy;
}

json RetryPolicyToJson(const RetryPolicy& policy) {
  json j = {{"max_attempts", policy.max_attempts},
            {"base_ns", policy.base_ns},
            {"multiplier", policy.multiplier},
            {"max_ns", policy.max_ns}};
  return j;
}

std::vector<std::string> ToStringVector(const json& j) {
  std::vector<std::string> out;
  out.reserve(j.size());
  for (const auto& item : j) {
    out.push_back(item.get<std::string>());
  }
  return out;
}

}  // namespace

std::string TaskStatusToString(TaskStatus status) {
  return TaskStatusName(status);
}

TaskStatus TaskStatusFromString(const std::string& name) {
  return ParseTaskStatus(name);
}

std::string CancellationPolicyToString(CancellationPolicy policy) {
  switch (policy) {
    case CancellationPolicy::kBestEffort:
      return "best_effort";
    default:
      return "cooperative";
  }
}

CancellationPolicy CancellationPolicyFromString(const std::string& name) {
  if (name == "best_effort") {
    return CancellationPolicy::kBestEffort;
  }
  if (name == "cooperative") {
    return CancellationPolicy::kCooperative;
  }
  throw ValidationError(ErrorCode::kValidationDomain,
                        "unknown cancellation policy: " + name);
}

std::string CachePolicyToString(CachePolicy policy) {
  switch (policy) {
    case CachePolicy::kNever:
      return "never";
    default:
      return "cacheable";
  }
}

CachePolicy CachePolicyFromString(const std::string& name) {
  if (name == "never") {
    return CachePolicy::kNever;
  }
  if (name == "cacheable") {
    return CachePolicy::kCacheable;
  }
  throw ValidationError(ErrorCode::kValidationDomain,
                        "unknown cache policy: " + name);
}

std::string FailurePolicyToString(FailurePolicy policy) {
  switch (policy) {
    case FailurePolicy::kFailed:
      return "failed";
    default:
      return "skipped";
  }
}

FailurePolicy FailurePolicyFromString(const std::string& name) {
  if (name == "failed") {
    return FailurePolicy::kFailed;
  }
  if (name == "skipped") {
    return FailurePolicy::kSkipped;
  }
  throw ValidationError(ErrorCode::kValidationDomain,
                        "unknown failure policy: " + name);
}

std::string TaskToJson(const TaskInstance& task) {
  json j;
  j["id"] = core::FormatUuid(task.id);
  j["definition"] = {
      {"type", task.definition.type},
      {"inputs",
       {{"artifact_kinds", task.definition.inputs.artifact_kinds},
        {"allow_external", task.definition.inputs.allow_external}}},
      {"outputs", {{"artifact_kinds", task.definition.outputs.artifact_kinds}}},
      {"parameters_schema_json", task.definition.parameters_schema_json},
      {"requirements", ResourceSpecToJson(task.definition.requirements)}};
  j["inputs"] = task.inputs;
  j["outputs"] = task.outputs;
  j["config_json"] = task.config_json;
  j["state"] = TaskStatusName(task.state);
  j["metadata"] = {
      {"retry", RetryPolicyToJson(task.metadata.retry)},
      {"cancellation", CancellationPolicyToString(task.metadata.cancellation)},
      {"cache", CachePolicyToString(task.metadata.cache)},
      {"failure", FailurePolicyToString(task.metadata.failure)},
      {"deterministic", task.metadata.deterministic},
      {"attempts", task.metadata.attempts},
      {"created_at_ns", task.metadata.created_at_ns},
      {"updated_at_ns", task.metadata.updated_at_ns}};
  return j.dump();
}

TaskInstance TaskFromJson(const std::string& json_str) {
  const json j = json::parse(json_str, nullptr, /*allow_exceptions=*/true);
  TaskInstance task;
  task.id = ParseUuid(j.at("id").get<std::string>());
  const json& def = j.at("definition");
  task.definition.type = def.at("type").get<std::string>();
  const json& in = def.at("inputs");
  task.definition.inputs.artifact_kinds = ToStringVector(in.at("artifact_kinds"));
  task.definition.inputs.allow_external = in.value("allow_external", false);
  const json& out = def.at("outputs");
  task.definition.outputs.artifact_kinds = ToStringVector(out.at("artifact_kinds"));
  task.definition.parameters_schema_json =
      def.value("parameters_schema_json", "");
  task.definition.requirements = ResourceSpecFromJson(def.at("requirements"));
  task.inputs = ToStringVector(j.at("inputs"));
  task.outputs = ToStringVector(j.at("outputs"));
  task.config_json = j.value("config_json", "");
  task.state = ParseTaskStatus(j.value("state", "pending"));
  const json& meta = j.at("metadata");
  task.metadata.retry = RetryPolicyFromJson(meta.at("retry"));
  task.metadata.cancellation =
      CancellationPolicyFromString(meta.value("cancellation", "cooperative"));
  task.metadata.cache = CachePolicyFromString(meta.value("cache", "cacheable"));
  task.metadata.failure = FailurePolicyFromString(meta.value("failure", "skipped"));
  task.metadata.deterministic = meta.value("deterministic", false);
  task.metadata.attempts = meta.value("attempts", 0);
  task.metadata.created_at_ns = meta.value("created_at_ns", std::int64_t{0});
  task.metadata.updated_at_ns = meta.value("updated_at_ns", std::int64_t{0});
  return task;
}

}  // namespace spatial::engine
