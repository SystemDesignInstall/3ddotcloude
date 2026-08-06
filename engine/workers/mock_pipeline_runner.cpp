#include "engine/workers/mock_pipeline_runner.h"

#include <cstdint>
#include <string>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::GenerateUuid;
using spatial::core::fs::Iso8601UtcNow;

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
    // Deterministic payload: task identity + effective configuration + real
    // input refs. Never includes the task id (that would break caching).
    std::string content = request.task_type + "|" + request.config_json;
    for (const auto& ref : request.input_refs) {
      content += "|" + ref;
    }
    const std::vector<std::uint8_t> payload(content.begin(), content.end());

    ArtifactManifest manifest;
    manifest.artifact_uuid = GenerateUuid();
    manifest.type = request.task_type;
    manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
    manifest.creation_timestamp = Iso8601UtcNow();
    manifest.file_size = static_cast<std::int64_t>(payload.size());
    manifest.mime_type = "application/octet-stream";

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
  };
}

}  // namespace spatial::engine
