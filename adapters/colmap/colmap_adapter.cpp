#include "adapters/colmap/colmap_adapter.h"

#include <cstddef>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/errors/project_error.h"

namespace spatial::adapters::colmap {

namespace {

const char* const kCapabilities[] = {
    "feature_extraction",
    "sparse_reconstruction",
    "bundle_adjustment",
};

}  // namespace

ColmapAdapter::ColmapAdapter(std::string executable)
    : executable_(std::move(executable)) {
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
  descriptor.input_artifact_kinds = {"image", "calibration"};
  descriptor.output_artifact_kinds = {"feature", "sparse_model"};
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
  // never linked. A real child-process spawn replaces this synchronous shell
  // probe when the worker lands.
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

void ColmapAdapter::Execute(const std::vector<std::string>& plan,
                            spatial::adapters::ResultSink& sink) {
  // C1-S2: the Execute contract is established here — validate the plan and
  // stream deterministic substage progress into the sink. Invoking the COLMAP
  // CLI (feature_extractor / matcher / mapper) is the next increment's
  // concern; this implementation never runs the backend and never fabricates
  // artifacts.
  const std::unordered_set<std::string> known_stages = {
      ColmapStageName(ColmapStage::kFeatureExtractor),
      ColmapStageName(ColmapStage::kMatcher),
      ColmapStageName(ColmapStage::kMapper),
  };
  if (plan.empty()) {
    throw spatial::core::AdapterError(
        spatial::core::ErrorCode::kValidationDomain,
        "empty COLMAP plan: Execute requires the plan produced by CreatePlan",
        {}, /*recoverable=*/false,
        "Run CreatePlan on the task request first; a task with no stage "
        "cannot execute.");
  }
  const std::size_t total = plan.size();
  for (std::size_t i = 0; i < total; ++i) {
    if (known_stages.count(plan[i]) == 0) {
      throw spatial::core::AdapterError(
          spatial::core::ErrorCode::kValidationDomain,
          "unknown COLMAP plan step '" + plan[i] + "'", {},
          /*recoverable=*/false,
          "CreatePlan produced a step outside the COLMAP stage vocabulary.");
    }
    const int percent = static_cast<int>((i + 1) * 100 / total);
    sink.Progress(percent, plan[i]);
    sink.Log("stage '" + plan[i] + "' planned: " + std::to_string(percent) +
             "%");
  }
}

}  // namespace spatial::adapters::colmap
