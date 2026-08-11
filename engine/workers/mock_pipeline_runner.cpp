#include "engine/workers/mock_pipeline_runner.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_manifest.h"
#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/feature_extraction.h"
#include "engine/pipeline/quality/quality_report.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ErrorCode;
using spatial::core::GenerateUuid;
using spatial::core::ProjectError;
using spatial::core::Sha256Hex;
using spatial::core::fs::Iso8601UtcNow;
using nlohmann::json;

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

// Fixed-size mock keypoint grid (RFC-0007 §2): detector "mock",
// descriptor_type "mock_16". The PRNG is seeded from the input image bytes'
// SHA-256, so identical images always yield identical keypoints and
// descriptors — and therefore an identical CAS hash (AC-8, ADR-020 replay).
// keypoint_count (1..4096, default 64) is read from the stage config's
// "config" object, e.g. {"config": {"keypoint_count": 128}}.
std::pair<std::vector<FeaturePoint>, std::vector<std::vector<double>>>
DeriveMockFeatures(const std::vector<std::uint8_t>& image_bytes,
                   const std::string& config_json) {
  std::size_t count = 64;
  if (!config_json.empty()) {
    try {
      const json cfg = json::parse(config_json);
      if (cfg.contains("config") && cfg["config"].is_object() &&
          cfg["config"].contains("keypoint_count") &&
          cfg["config"]["keypoint_count"].is_number_integer()) {
        const std::int64_t n =
            cfg["config"]["keypoint_count"].get<std::int64_t>();
        if (n > 0 && n <= 4096) {
          count = static_cast<std::size_t>(n);
        }
      }
    } catch (const json::parse_error&) {
      // The compiler pre-validates config, but a direct caller must not
      // crash; fall back to the default count.
    }
  }

  // Seed the PRNG from the image content itself (pure function of the bytes).
  const std::string digest = Sha256Hex(image_bytes);
  std::uint64_t state = 0;
  for (std::size_t i = 0; i + 8 <= digest.size(); i += 8) {
    std::uint64_t word = 0;
    for (int b = 0; b < 8; ++b) {
      word = (word << 8) | static_cast<std::uint8_t>(digest[i + b]);
    }
    state ^= word;
  }
  if (state == 0) {
    state = 0x9e3779b97f4a7c15ull;
  }
  const auto next = [&state]() {
    // xorshift64* — cheap and deterministic across platforms.
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 0x2545F4914F6CDD1Dull;
  };

  const std::size_t side = static_cast<std::size_t>(
      std::ceil(std::sqrt(static_cast<double>(count))));
  const double spacing = side > 0 ? 96.0 / static_cast<double>(side) : 96.0;
  const auto jitter = [&next]() {
    return static_cast<double>(next() % 1000) / 1000.0;
  };

  std::vector<FeaturePoint> keypoints;
  keypoints.reserve(count);
  std::vector<std::vector<double>> descriptors;
  descriptors.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    FeaturePoint p;
    p.x = (i % side) * spacing + jitter() * spacing * 0.25;
    p.y = (i / side) * spacing + jitter() * spacing * 0.25;
    p.size = 2.0 + jitter() * 4.0;
    p.angle = static_cast<double>(next() % 62831) / 100.0;
    p.response = static_cast<double>(next() % 100000) / 100000.0;
    keypoints.push_back(p);

    std::vector<double> row;
    row.reserve(16);
    for (int d = 0; d < 16; ++d) {
      row.push_back(static_cast<double>(next() % 65536) / 65536.0);
    }
    descriptors.push_back(std::move(row));
  }
  return {std::move(keypoints), std::move(descriptors)};
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

    // RFC-0007 §6: the single-stage Feature Extraction pipeline. The stage
    // genuinely reads the input image bytes and derives a deterministic
    // mock feature payload from them, then writes the canonical
    // FeatureArtifact (type "feature", feature.schema.json) via the
    // scene-agnostic payload writer (ADR-038). The per-frame feature_sets
    // row is recorded at the session/CLI layer (§8). A missing input image
    // is a domain error: throw, which the in-process executor surfaces as
    // kFailed.
    if (request.task_type == "feature_extract") {
      if (request.input_refs.empty()) {
        throw ProjectError(ErrorCode::kValidationDomain,
                           "feature_extract: missing input image ref");
      }
      const auto image = store.Get(request.input_refs.front());
      if (!image) {
        throw ProjectError(
            ErrorCode::kValidationDomain,
            "feature_extract: input image '" + request.input_refs.front() +
                "' is not in the artifact store");
      }

      auto features = DeriveMockFeatures(*image, request.config_json);

      WriteFeatureArtifactInput input;
      input.detector = "mock";
      input.descriptor_type = "mock_16";
      input.input_content_hash = request.input_refs.front();
      input.keypoints = std::move(features.first);
      input.descriptors = std::move(features.second);

      const auto result = WriteFeatureArtifactPayload(store, input);

      WorkerEvent produced;
      produced.type = WorkerEventType::kArtifactProduced;
      produced.task_id = request.task_id;
      produced.artifact_ref = result.content_hash;
      emit(produced);

      WorkerEvent completed;
      completed.type = WorkerEventType::kCompleted;
      completed.task_id = request.task_id;
      emit(completed);
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
