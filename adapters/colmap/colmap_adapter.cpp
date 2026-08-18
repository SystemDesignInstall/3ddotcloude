#include "adapters/colmap/colmap_adapter.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "adapters/colmap/colmap_cli.h"
#include "adapters/colmap/colmap_converter.h"
#include "adapters/process/process_runner.h"
#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/errors/project_error.h"
#include "core/reconstruction/reconstruction.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"

namespace spatial::adapters::colmap {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ErrorCode;
using spatial::core::fs::AtomicWrite;
using spatial::core::fs::CreateDirectories;
using spatial::core::fs::Iso8601UtcNow;
using spatial::core::fs::ReadFile;
using spatial::core::GenerateUuid;
using spatial::core::FormatUuid;
using spatial::core::Sha256Hex;
using spatial::core::ToJsonString;

const char* const kCapabilities[] = {
    "feature_extraction",
    "sparse_reconstruction",
    "bundle_adjustment",
};

const char* const kInputKinds[] = {"image", "calibration"};

// A bounded tail of stderr for diagnostics (the full stream is streamed to
// the sink as TaskLog before any failure is raised).
std::string StderrExcerpt(const std::string& stderr_text) {
  if (stderr_text.empty()) {
    return "";
  }
  const std::string trimmed = stderr_text;
  const std::string head = trimmed.size() > 400 ? "..." + trimmed.substr(trimmed.size() - 400)
                                                : trimmed;
  return "\nstderr: " + head;
}

std::string CommandSummary(const std::vector<std::string>& argv) {
  std::string summary;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      summary += ' ';
    }
    summary += argv[i];
  }
  return summary;
}

std::vector<std::string> SortedUnique(std::vector<std::string> refs) {
  std::sort(refs.begin(), refs.end());
  refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
  return refs;
}

}  // namespace

ColmapAdapter::ColmapAdapter(std::string executable,
                             std::shared_ptr<const ExecutionContext> context)
    : executable_(std::move(executable)), context_(std::move(context)) {
  profile_.name = "colmap-cpu";
  profile_.capabilities.assign(std::begin(kCapabilities),
                               std::end(kCapabilities));
  profile_.capacity.cores = 4;
  profile_.capacity.ram_bytes = 8LL * 1024 * 1024 * 1024;
  profile_.capacity.gpus = 0;
  profile_.capacity.gpu_mem_bytes = 0;
  profile_.capacity.temp_disk_bytes = 20LL * 1024 * 1024 * 1024;
  profile_.max_concurrency = 1;
}

spatial::adapters::AdapterDescriptor ColmapAdapter::Descriptor() const {
  spatial::adapters::AdapterDescriptor descriptor;
  descriptor.adapter_id = "colmap";
  descriptor.version = kColmapAdapterVersion;
  descriptor.git_commit = kColmapAdapterGitCommit;
  descriptor.capabilities.assign(std::begin(kCapabilities),
                                 std::end(kCapabilities));
  descriptor.license_ref = "COLMAP";  // THIRD_PARTY.yml key (BSD-3-Clause)
  descriptor.profile = profile_;
  descriptor.input_artifact_kinds.assign(std::begin(kInputKinds),
                                         std::end(kInputKinds));
  descriptor.output_artifact_kinds = {"feature", "reconstruction"};
  return descriptor;
}

bool ColmapAdapter::ValidateEnvironment(std::string& problem) const {
#if defined(_WIN32)
  const char* kNullDevice = "nul";
#else
  const char* kNullDevice = "/dev/null";
#endif
  // Doctor probe (adding-adapter.md §5): the backend is runnable here iff the
  // external executable answers `--version` successfully. COLMAP is launched,
  // never linked.
  const std::string command =
      "\"" + executable_ + "\" --version >" + kNullDevice + " 2>&1";
  const int exit_code = std::system(command.c_str());
  if (exit_code == 0) {
    return true;
  }
  problem = "COLMAP executable is not runnable here: '" + executable_ +
            "' returned exit code " + std::to_string(exit_code) +
            " for `--version`. Install COLMAP and ensure it is on PATH, or "
            "configure the adapter's executable path.";
  return false;
}

std::vector<std::string> ColmapAdapter::CreatePlan(
    const spatial::engine::TaskRequest& request) const {
  // FromJson rejects malformed JSON, unknown keys/stages, and any calibration
  // vocabulary in the configuration surface (RFC-0009 §6). Planning is pure
  // reasoning: it never runs the backend.
  const ColmapConfig config = ColmapConfig::FromJson(request.config_json);
  return config.Plan();
}

void ColmapAdapter::MaterializeInputs(
    const std::filesystem::path& workspace) {
  if (context_->input_refs.empty()) {
    return;
  }
  if (context_->store == nullptr) {
    // Worker path (C1-S4): the host already materialized every declared input
    // into workspace/inputs/<hash> (ProcessExecutor); the worker has no CAS
    // access (ADR-038). Stage the local files into the CLI layout and fail
    // closed when a declared input is missing from the workspace.
    const auto inputs_dir = workspace / "inputs";
    for (std::size_t i = 0; i < context_->input_refs.size(); ++i) {
      const std::string& ref = context_->input_refs[i];
      const std::string kind =
          i < context_->input_kinds.size() ? context_->input_kinds[i] : "image";
      if (kind != "image" && kind != "calibration") {
        throw spatial::core::AdapterError(
            ErrorCode::kAdapterProcessFailed,
            "unknown input kind '" + kind + "' for input " + ref, {},
            /*recoverable=*/false,
            "Input kinds are restricted to the declared "
            "input_artifact_kinds {image, calibration}.");
      }
      std::vector<std::uint8_t> bytes;
      try {
        bytes = ReadFile(inputs_dir / ref);
      } catch (const std::exception&) {
        throw spatial::core::AdapterError(
            ErrorCode::kAdapterProcessFailed,
            "input not materialized in workspace: " + ref, {},
            /*recoverable=*/false,
            "The engine materializes TaskRequest.input_refs into "
            "workspace/inputs/<hash> before dispatch; a missing file means the "
            "host-side materialization failed or the ref was never declared.");
      }
      if (kind == "image") {
        AtomicWrite(workspace / "images" / ref, bytes);
      } else {
        AtomicWrite(workspace / "calibration.json", bytes);
      }
    }
    return;
  }
  const auto inputs_dir = workspace / "inputs";
  CreateDirectories(inputs_dir);
  for (std::size_t i = 0; i < context_->input_refs.size(); ++i) {
    const std::string& ref = context_->input_refs[i];
    const std::string kind =
        i < context_->input_kinds.size() ? context_->input_kinds[i] : "image";
    if (kind != "image" && kind != "calibration") {
      throw spatial::core::AdapterError(
          ErrorCode::kAdapterProcessFailed,
          "unknown input kind '" + kind + "' for input " + ref, {},
          /*recoverable=*/false,
          "Input kinds are restricted to the declared "
          "input_artifact_kinds {image, calibration}.");
    }
    std::optional<std::vector<std::uint8_t>> bytes;
    try {
      bytes = context_->store->Get(ref);
    } catch (const spatial::core::ProjectError&) {
      throw spatial::core::AdapterError(
          ErrorCode::kAdapterProcessFailed,
          "input corrupt in CAS (SHA-256 verification failed): " + ref, {},
          /*recoverable=*/false,
          "Re-import the input artifact; its payload failed content-hash "
          "verification.");
    }
    if (!bytes) {
      throw spatial::core::AdapterError(
          ErrorCode::kAdapterProcessFailed, "input not in CAS: " + ref, {},
          /*recoverable=*/false,
          "Provide the input artifact to the session before running the task.");
    }
    // Materialize to local files: the CLI receives paths, never content
    // hashes (RFC-0009 §6, worker-protocol §4).
    AtomicWrite(inputs_dir / ref, *bytes);
    if (kind == "image") {
      AtomicWrite(workspace / "images" / ref, *bytes);
    } else {
      AtomicWrite(workspace / "calibration.json", *bytes);
    }
  }
}

std::string ColmapAdapter::BuildManifest(
    const std::filesystem::path& payload) {
  const std::vector<std::uint8_t> bytes = ReadFile(payload);
  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "reconstruction";
  manifest.schema_version = 2;
  manifest.producer.id = "colmap";
  manifest.producer.version = kColmapAdapterVersion;
  manifest.producer.git_commit = kColmapAdapterGitCommit;
  manifest.input_artifact_hashes = SortedUnique(context_->input_refs);
  manifest.configuration_hash = Sha256Hex(context_->config_json);
  manifest.creation_timestamp = Iso8601UtcNow();
  manifest.coordinate_frame = "world";
  manifest.unit = "meter";
  manifest.content_hash = Sha256Hex(bytes);
  manifest.file_size = static_cast<std::int64_t>(bytes.size());
  manifest.mime_type = "application/octet-stream";
  manifest.validation_status = "valid";
  return ToJsonString(manifest);
}

void ColmapAdapter::Execute(const std::vector<std::string>& plan,
                            spatial::adapters::ResultSink& sink) {
  const std::unordered_set<std::string> known_stages = {
      ColmapStageName(ColmapStage::kFeatureExtractor),
      ColmapStageName(ColmapStage::kMatcher),
      ColmapStageName(ColmapStage::kMapper),
  };
  if (plan.empty()) {
    throw spatial::core::AdapterError(
        ErrorCode::kValidationDomain,
        "empty COLMAP plan: Execute requires the plan produced by CreatePlan",
        {}, /*recoverable=*/false,
        "Run CreatePlan on the task request first; a task with no stage "
        "cannot execute.");
  }
  for (const std::string& step : plan) {
    if (known_stages.count(step) == 0) {
      throw spatial::core::AdapterError(
          ErrorCode::kValidationDomain,
          "unknown COLMAP plan step '" + step + "'", {},
          /*recoverable=*/false,
          "CreatePlan produced a step outside the COLMAP stage vocabulary.");
    }
  }

  if (context_ == nullptr) {
    throw spatial::core::AdapterError(
        ErrorCode::kAdapterProcessFailed,
        "no execution context: Execute requires an ExecutionContext "
        "(workspace, inputs, CAS store) bound to the task",
        {}, /*recoverable=*/false,
        "Bind an ExecutionContext to the adapter before execution; the COLMAP "
        "worker builds one per task.");
  }

  // The scheduler hands workers relative workspaces (temp/<job>/<task>,
  // resolved against the host cwd, scheduler.cpp). Canonicalize to an
  // absolute path before building any CLI argv: the backend resolves
  // --image_path / --output_path against ITS working directory — which is
  // the workspace itself — so a relative path would double-nest and break
  // every stage. Idempotent for already-absolute workspaces.
  const std::filesystem::path workspace =
      std::filesystem::absolute(context_->workspace);

  // Isolated task workspace (plan §4) + input materialization (RFC-0009 §6).
  CreateWorkspace(workspace);
  MaterializeInputs(workspace);

  // Effective configuration — the same document CreatePlan validated.
  const ColmapConfig config = ColmapConfig::FromJson(context_->config_json);

  const std::size_t total = plan.size();
  for (std::size_t i = 0; i < total; ++i) {
    const ColmapStage stage = *ColmapStageFromName(plan[i]);
    if (context_->cancel != nullptr && context_->cancel->cancelled()) {
      throw spatial::core::AdapterError(
          ErrorCode::kAdapterProcessCancelled,
          "task cancelled before stage '" + plan[i] + "' ran", {},
          /*recoverable=*/false,
          "The task was cancelled at a CLI-tool boundary; it is never re-run "
          "automatically.");
    }
    const std::vector<std::string> argv =
        BuildStageCommand(executable_, stage, workspace, config);
    sink.Log("executing " + CommandSummary(argv));

    const spatial::adapters::process::ProcessSpec spec{argv, workspace, {}};
    const spatial::adapters::process::ProcessResult result =
        spatial::adapters::process::RunSubprocess(
            spec, context_->stage_timeout_ms, context_->cancel);

    switch (result.outcome) {
      case spatial::adapters::process::ProcessOutcome::kSpawnFailed:
        throw spatial::core::AdapterError(
            ErrorCode::kAdapterProcessFailed,
            "failed to start " + StageSubcommand(stage) + ": " +
                result.error_message,
            {}, /*recoverable=*/false,
            "Ensure the COLMAP executable is installed and runnable (doctor "
            "step, ValidateEnvironment).");
      case spatial::adapters::process::ProcessOutcome::kTimedOut:
        throw spatial::core::AdapterError(
            ErrorCode::kAdapterProcessTimeout,
            StageSubcommand(stage) + " exceeded the " +
                std::to_string(context_->stage_timeout_ms) +
                " ms time limit" + StderrExcerpt(result.stderr_text),
            {}, /*recoverable=*/true,
            "Increase the stage time limit or reduce the workload; a timed-out "
            "stage is retried per the task's retry policy.");
      case spatial::adapters::process::ProcessOutcome::kCancelled:
        throw spatial::core::AdapterError(
            ErrorCode::kAdapterProcessCancelled,
            StageSubcommand(stage) + " was cancelled", {},
            /*recoverable=*/false,
            "The task was cancelled at a CLI-tool boundary; it is never "
            "re-run automatically.");
      case spatial::adapters::process::ProcessOutcome::kCompleted:
        break;
    }

    if (result.exit_code != 0) {
      throw spatial::core::AdapterError(
          ErrorCode::kAdapterProcessFailed,
          StageSubcommand(stage) + " exited with code " +
              std::to_string(result.exit_code) +
              StderrExcerpt(result.stderr_text),
          {}, /*recoverable=*/false,
          "Inspect the COLMAP diagnostics above; fix the inputs or "
          "configuration and re-run.");
    }

    sink.Progress(static_cast<int>((i + 1) * 100 / total), plan[i]);
    if (!result.stdout_text.empty()) {
      sink.Log(result.stdout_text);
    }
    if (!result.stderr_text.empty()) {
      sink.Log(result.stderr_text);
    }
  }

  // Output discovery -> canonical Reconstruction v2 artifact. The only reader
  // of COLMAP's native binary formats is the converter (RFC-0008 §6/§7): native
  // cameras/images/points3D.bin -> SparseModel -> canonical Reconstruction v2
  // -> workspace payload -> provenance manifest. The host verifies and ingests
  // into the CAS.
  const std::vector<std::filesystem::path> native_files =
      DiscoverNativeModelFiles(workspace);
  if (!native_files.empty()) {
    const SparseModel model = ParseSparseModel(
        native_files[0], native_files[1], native_files[2]);

    // Build provenance info for v2 output.
    ReconstructionProvenanceInfo prov_info;
    prov_info.backend_name = "colmap";
    prov_info.backend_version = "";  // COLMAP version not readily available here
    prov_info.adapter_version = kColmapAdapterVersion;
    prov_info.git_commit = kColmapAdapterGitCommit;
    prov_info.configuration_hash = Sha256Hex(context_->config_json);
    prov_info.input_artifact_hashes = SortedUnique(context_->input_refs);

    // Scene/session IDs are not available at adapter level; the downstream
    // consumer (engine/worker) is responsible for linking the reconstruction
    // to its scene and sessions via the metadata DB.
    const std::string reconstruction_id = FormatUuid(GenerateUuid());

    spatial::core::Reconstruction rec = SparseModelToReconstruction(
        model,
        /*reconstruction_id=*/reconstruction_id,
        /*scene_id=*/"",
        /*session_ids=*/std::vector<std::string>{},
        /*coordinate_frame=*/"reconstruction_0",
        /*provenance=*/prov_info);

    const std::filesystem::path payload =
        SparseModelDir(workspace) / "reconstruction.json";
    AtomicWrite(payload, ReconstructionToJson(rec));
    sink.ArtifactProduced(payload.string(), BuildManifest(payload));
  }
}

}  // namespace spatial::adapters::colmap
