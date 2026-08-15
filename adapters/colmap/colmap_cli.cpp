#include "adapters/colmap/colmap_cli.h"

#include <fstream>
#include <system_error>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"

namespace spatial::adapters::colmap {
namespace {

using spatial::core::fs::CreateDirectories;
using spatial::core::fs::Exists;

std::vector<std::string> StageArgTokens(const ColmapConfig& config,
                                        ColmapStage stage) {
  std::vector<std::string> tokens = config.BuildStageArgs(stage);
  tokens.push_back("--threads");
  tokens.push_back(std::to_string(config.threads));
  if (!config.seed.empty()) {
    tokens.push_back("--random_seed");
    tokens.push_back(config.seed);
  }
  return tokens;
}

}  // namespace

std::string StageSubcommand(ColmapStage stage) {
  switch (stage) {
    case ColmapStage::kFeatureExtractor:
      return "feature_extractor";
    case ColmapStage::kMatcher:
      return "exhaustive_matcher";
    case ColmapStage::kMapper:
      return "mapper";
  }
  return "unknown";
}

void CreateWorkspace(const std::filesystem::path& workspace) {
  if (workspace.empty()) {
    throw spatial::core::AdapterError(
        spatial::core::ErrorCode::kAdapterProcessFailed,
        "empty COLMAP workspace path", {}, /*recoverable=*/false,
        "Configure the task workspace before executing the adapter.");
  }
  for (const char* dir : {"images", "features", "matches", "sparse", "logs"}) {
    CreateDirectories(workspace / dir);
  }
  const auto db = workspace / "database.db";
  if (!Exists(db)) {
    std::ofstream touch(db, std::ios::binary);
    touch.close();
  }
}

std::vector<std::string> BuildStageCommand(
    const std::string& executable, ColmapStage stage,
    const std::filesystem::path& workspace, const ColmapConfig& config) {
  const auto ws = workspace.string();
  std::vector<std::string> command = {executable, StageSubcommand(stage)};
  switch (stage) {
    case ColmapStage::kFeatureExtractor:
      command.push_back("--database_path");
      command.push_back(ws + "/database.db");
      command.push_back("--image_path");
      command.push_back(ws + "/images");
      break;
    case ColmapStage::kMatcher:
      command.push_back("--database_path");
      command.push_back(ws + "/database.db");
      break;
    case ColmapStage::kMapper:
      command.push_back("--output_path");
      command.push_back(ws + "/sparse");
      break;
  }
  const std::vector<std::string> tokens = StageArgTokens(config, stage);
  command.insert(command.end(), tokens.begin(), tokens.end());
  return command;
}

std::filesystem::path SparseModelDir(
    const std::filesystem::path& workspace) {
  return workspace / "sparse" / "0";
}

std::vector<std::filesystem::path> DiscoverNativeModelFiles(
    const std::filesystem::path& workspace) {
  const std::filesystem::path model_dir = SparseModelDir(workspace);
  const std::vector<std::filesystem::path> files = {
      model_dir / "cameras.bin",
      model_dir / "images.bin",
      model_dir / "points3D.bin",
  };
  std::size_t present = 0;
  for (const auto& file : files) {
    if (Exists(file)) {
      ++present;
    }
  }
  if (present == 0) {
    return {};  // the mapper produced no reconstruction
  }
  if (present != files.size()) {
    throw spatial::core::AdapterError(
        spatial::core::ErrorCode::kAdapterProcessFailed,
        "incomplete COLMAP model in " + model_dir.string() +
            ": expected cameras.bin, images.bin, points3D.bin",
        {}, /*recoverable=*/false,
        "Re-run the reconstruction; a partial sparse model is never "
        "converted or emitted.");
  }
  return files;
}

}  // namespace spatial::adapters::colmap
