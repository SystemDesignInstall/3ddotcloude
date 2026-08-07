#include "engine/workers/mock_pipeline_runner.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/quality/quality_report.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::GenerateUuid;
using spatial::core::fs::Iso8601UtcNow;

// Writes `payload` to the store, emits the produced/completed events, and
// returns. Shared by every stage so the event stream stays uniform.
void EmitResult(spatial::core::ArtifactStore& store,
                const TaskRequest& request,
                const std::vector<std::uint8_t>& payload,
                const std::string& artifact_type,
                std::vector<ArtifactRef> input_hashes,
                const std::function<void(WorkerEvent)>& emit) {
  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = artifact_type;
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.input_artifact_hashes = std::move(input_hashes);
  manifest.creation_timestamp = Iso8601UtcNow();
  manifest.file_size = static_cast<std::int64_t>(payload.size());
  manifest.mime_type =
      artifact_type == "quality_report" ? "application/json"
                                        : "application/octet-stream";

  const auto result = store.Put(payload, manifest);

  WorkerEvent produced;
  produced.type = WorkerEventType::kArtifactProduced;
  produced.task_id = request.task_id;
  produced.artifact_ref = result.content_hash;
  emit(produced);

  WorkerEvent completed;
  completed.type = WorkerEventType::kCompleted;
  completed.task_id = request.task_id;
  emit(completed);
}

}  // namespace

ResourceProfile DemoWorkerProfile() {
  ResourceProfile profile;
  profile.name = "demo-inprocess";
  profile.capabilities = {"feature_extraction", "reconstruction",
                          "validation"};
  profile.capacity.cores = 4;
  profile.capacity.ram_bytes = std::int64_t{1} << 30;
  profile.capacity.temp_disk_bytes = std::int64_t{1} << 30;
  profile.max_concurrency = 1;
  return profile;
}

InProcessTaskRunner MakeMockPipelineRunner(spatial::core::ArtifactStore& store) {
  return [&store](const TaskRequest& request,
                  const std::function<void(WorkerEvent)>& emit,
                  const std::function<bool()>& /*cancelled*/) {
    // RFC-0005: the terminal `validate` stage runs the quality engine and
    // produces a quality_report artifact. Evaluation is a pure function of
    // the run, so the produced bytes (and hence the CAS hash) are stable
    // across runs and cache replays (AC-8).
    if (request.task_type == "validate") {
      const QualityReport report = EvaluateQuality(
          request.pipeline_hash, "validate", request.config_json,
          request.input_refs);
      const std::string report_json = QualityReportToJson(report);
      const std::vector<std::uint8_t> payload(report_json.begin(),
                                              report_json.end());
      EmitResult(store, request, payload, "quality_report",
                 request.input_refs, emit);
      return;
    }

    // Deterministic payload: task identity + effective configuration + real
    // input refs. Never includes the task id (that would break caching).
    std::string content = request.task_type + "|" + request.config_json;
    for (const auto& ref : request.input_refs) {
      content += "|" + ref;
    }
    const std::vector<std::uint8_t> payload(content.begin(), content.end());
    EmitResult(store, request, payload, request.task_type, {}, emit);
  };
}

}  // namespace spatial::engine
