#include "engine/execution/execution_record.h"

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/uuid.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::ParseUuid;
using spatial::core::ValidationError;
using nlohmann::json;

Uuid UuidOrNil(const json& j) {
  return j.is_null() ? Uuid{} : ParseUuid(j.get<std::string>());
}

}  // namespace

std::string ToJson(const SoftwareEnvironment& environment) {
  json j = {{"engine_version", environment.engine_version},
            {"git_commit", environment.git_commit},
            {"protocol_version", environment.protocol_version}};
  return j.dump();
}

std::string ToJson(const HardwareInfo& hardware) {
  json j = {{"os", hardware.os},
            {"arch", hardware.arch},
            {"cpu", hardware.cpu},
            {"ram_bytes", hardware.ram_bytes},
            {"gpu", hardware.gpu}};
  return j.dump();
}

std::string ToJson(const ExecutionRecord& record) {
  json j;
  j["id"] = core::FormatUuid(record.id);
  j["task_id"] = core::FormatUuid(record.task_id);
  j["attempt"] = record.attempt;
  if (record.worker_id) {
    j["worker_id"] = core::FormatUuid(*record.worker_id);
  } else {
    j["worker_id"] = nullptr;
  }
  j["inputs"] = record.inputs;
  j["outputs"] = record.outputs;
  j["environment"] = json::parse(ToJson(record.environment));
  j["hardware"] = json::parse(ToJson(record.hardware));
  j["started_at_ns"] = record.started_at_ns;
  j["ended_at_ns"] = record.ended_at_ns;
  j["terminal_state"] = TaskStatusName(record.terminal_state);
  if (record.error) {
    j["error"] = {{"code", record.error->code},
                  {"message", record.error->message},
                  {"recoverable", record.error->recoverable}};
  } else {
    j["error"] = nullptr;
  }
  return j.dump();
}

ExecutionRecord ExecutionRecordFromJson(const std::string& json_str) {
  const json j = json::parse(json_str, nullptr, /*allow_exceptions=*/true);
  ExecutionRecord record;
  record.id = ParseUuid(j.at("id").get<std::string>());
  record.task_id = ParseUuid(j.at("task_id").get<std::string>());
  record.attempt = j.value("attempt", 1);
  record.worker_id = UuidOrNil(j.at("worker_id"));
  record.inputs = j.at("inputs").get<std::vector<std::string>>();
  record.outputs = j.at("outputs").get<std::vector<std::string>>();
  const json& env = j.at("environment");
  record.environment.engine_version = env.value("engine_version", "");
  record.environment.git_commit = env.value("git_commit", "");
  record.environment.protocol_version = env.value("protocol_version", "");
  const json& hw = j.at("hardware");
  record.hardware.os = hw.value("os", "");
  record.hardware.arch = hw.value("arch", "");
  record.hardware.cpu = hw.value("cpu", "");
  record.hardware.ram_bytes = hw.value("ram_bytes", std::int64_t{0});
  record.hardware.gpu = hw.value("gpu", "");
  record.started_at_ns = j.value("started_at_ns", std::int64_t{0});
  record.ended_at_ns = j.value("ended_at_ns", std::int64_t{0});
  record.terminal_state = ParseTaskStatus(j.value("terminal_state", "failed"));
  if (!j.at("error").is_null()) {
    ErrorRecord error;
    error.code = j.at("error").value("code", "");
    error.message = j.at("error").value("message", "");
    error.recoverable = j.at("error").value("recoverable", false);
    record.error = error;
  }
  return record;
}

}  // namespace spatial::engine
