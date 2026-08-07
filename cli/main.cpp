// spatial - the Core Platform CLI (RFC-0003 P1.4, AC-7). A thin client of the
// Engine composition root: it opens a project, registers input artifacts in
// CAS, and drives Engine::RunPipeline / RunGraph. Command surface:
//
//   spatial run <pipeline-id> [--config <json>] [--input <file> ...] [--project <dir>]
//   spatial run --dag <dag.json> [--project <dir>]
//   spatial status <run-id> [--project <dir>]

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "core/artifacts/artifact_manifest.h"
#include "core/artifacts/artifact_store.h"
#include "core/errors/project_error.h"
#include "core/project/project.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/mock_photogrammetry.h"
#include "engine/pipeline/quality/quality_report.h"
#include "engine/task/task_serialization.h"

namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactStore;
using spatial::core::GenerateUuid;
using spatial::core::ParseUuid;
using spatial::core::Project;
using spatial::core::ProjectError;
using spatial::core::Uuid;
using spatial::core::fs::Iso8601UtcNow;
using spatial::engine::ArtifactRef;
using spatial::engine::Engine;

constexpr const char* kUsage =
    "usage:\n"
    "  spatial init <name.spx> [--project <dir>]\n"
    "  spatial run <pipeline-id> [--config <json>] [--input <file> ...]"
    " [--project <dir>]\n"
    "  spatial run --dag <dag.json> [--project <dir>]\n"
    "  spatial status <run-id> [--project <dir>]\n"
    "  spatial report <run-id> [--project <dir>]\n";

void PrintError(const ProjectError& e) {
  std::cerr << "error: " << e.message() << "\n";
  if (!e.suggested_action().empty()) {
    std::cerr << "hint: " << e.suggested_action() << "\n";
  }
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw ProjectError(spatial::core::ErrorCode::kProjectInvalid,
                       "cannot open input file: " + path.string());
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

ArtifactRef RegisterInput(ArtifactStore& store,
                          const std::filesystem::path& path) {
  const auto bytes = ReadFileBytes(path);
  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "image";
  manifest.producer = {"spatial-platform", "cli", ""};
  manifest.creation_timestamp = Iso8601UtcNow();
  manifest.file_size = static_cast<std::int64_t>(bytes.size());
  manifest.mime_type = "application/octet-stream";
  return store.Put(bytes, manifest).content_hash;
}

// Consumes one or more consecutive flag values (stopping at the next flag).
std::vector<std::string> CollectValues(const std::vector<std::string>& args,
                                       std::size_t& i) {
  std::vector<std::string> values;
  while (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) {
    values.push_back(args[++i]);
  }
  if (values.empty()) {
    throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                       "missing value for " + args[i]);
  }
  return values;
}

// Splits `--project <dir>` out of the argument list (default: ".") and opens
// the project. The project flag is position-independent for every command;
// `args` is left with the remaining arguments in place.
Engine OpenProjectAndEngine(std::vector<std::string>& args) {
  std::filesystem::path project_dir = ".";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--project" && i + 1 < args.size()) {
      project_dir = args[i + 1];
      args.erase(args.begin() + i, args.begin() + i + 2);
      break;
    }
  }
  return Engine(Project::Open(project_dir));
}

int RunCommand(const std::vector<std::string>& args, Engine& engine,
               std::string pipeline_id) {
  std::vector<std::filesystem::path> inputs;
  std::string config;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--input") {
      for (const auto& value : CollectValues(args, i)) {
        inputs.emplace_back(value);
      }
    } else if (args[i] == "--config") {
      config = CollectValues(args, i).front();
    } else {
      throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                         "unknown argument: " + args[i]);
    }
  }
  if (inputs.empty()) {
    throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                       "pipeline '" + pipeline_id +
                           "' requires at least one --input file");
  }

  std::vector<ArtifactRef> refs;
  refs.reserve(inputs.size());
  for (const auto& input : inputs) {
    refs.push_back(RegisterInput(engine.project().artifacts(), input));
  }

  const auto manifest =
      engine.RunPipeline(pipeline_id, refs, config);
  std::cout << spatial::engine::ToJson(manifest) << "\n";
  return 0;
}

int RunDagCommand(const std::string& dag_path, Engine& engine) {
  std::ifstream in(dag_path, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open dag file: " << dag_path << "\n";
    return 1;
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  std::vector<ArtifactRef> external;
  auto graph = spatial::engine::TaskGraphFromJson(text, external);
  const Uuid job_id = engine.RunGraph(graph, external);
  std::cout << "run " << spatial::core::FormatUuid(job_id) << "\n";
  return 0;
}

// Renders the RFC-0005 quality report of a run: a human summary (verdict,
// score, metrics) followed by the canonical JSON document.
int ReportCommand(const Uuid& run_id, Engine& engine) {
  const auto manifest = engine.LoadManifest(run_id);
  if (!manifest) {
    std::cerr << "no such run: " << spatial::core::FormatUuid(run_id) << "\n";
    return 1;
  }
  if (!manifest->quality_report_id) {
    std::cerr << "no quality report for run "
              << spatial::core::FormatUuid(run_id) << "\n";
    return 1;
  }
  const auto artifact =
      engine.project().artifacts().ReadManifest(*manifest->quality_report_id);
  if (!artifact) {
    std::cerr << "quality report artifact is missing (uuid "
              << spatial::core::FormatUuid(*manifest->quality_report_id)
              << ")\n";
    return 1;
  }
  const auto payload = engine.project().artifacts().Get(artifact->content_hash);
  if (!payload) {
    std::cerr << "quality report payload is missing\n";
    return 1;
  }
  const std::string text(payload->begin(), payload->end());
  const auto report = spatial::engine::QualityReportFromJson(text);
  std::cout << "verdict: " << spatial::engine::QualityVerdictName(report.verdict)
            << "  score: " << report.quality_score << "/100\n"
            << "engine:  " << report.engine_name << " " << report.engine_version
            << " (" << report.engine_git_commit << ")\n"
            << "stage:   " << report.stage_id << "\n"
            << "metrics:\n"
            << "  reprojection  rmse " << report.reprojection.rmse_px << " px,"
            << " mean " << report.reprojection.mean_error_px << " px\n"
            << "  coverage      completeness "
            << report.coverage.completeness_pct << "%,"
            << " baseline " << report.coverage.baseline_ratio << "\n"
            << "  geometry      points " << report.geometry.point_count << ","
            << " confidence " << report.geometry.mean_confidence << "\n"
            << "document:\n"
            << text << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty() || args[0] == "--help" || args[0] == "-h") {
    std::cerr << kUsage;
    return 2;
  }

  try {
    if (args[0] == "init") {
      if (args.size() < 2) {
        std::cerr << kUsage;
        return 2;
      }
      spatial::core::ProjectInfo info;
      info.uuid = GenerateUuid();
      info.name = std::filesystem::path(args[1]).stem().string();
      info.created_at = Iso8601UtcNow();
      Project::Create(args[1], info);
      std::cout << "created " << args[1] << "\n";
      return 0;
    }

    if (args[0] == "run") {
      if (args.size() < 2) {
        std::cerr << kUsage;
        return 2;
      }
      std::vector<std::string> rest(args.begin() + 1, args.end());
      Engine engine = OpenProjectAndEngine(rest);
      if (rest[0] == "--dag") {
        if (rest.size() < 2) {
          std::cerr << kUsage;
          return 2;
        }
        return RunDagCommand(rest[1], engine);
      }
      // `spatial run <pipeline-id>`: everything after the id is flags.
      RegisterMockPhotogrammetry(engine.registry());
      const std::string pipeline_id = rest[0];
      std::vector<std::string> flags(rest.begin() + 1, rest.end());
      return RunCommand(flags, engine, pipeline_id);
    }

    if (args[0] == "status") {
      if (args.size() < 2) {
        std::cerr << kUsage;
        return 2;
      }
      std::vector<std::string> rest(args.begin() + 1, args.end());
      Engine engine = OpenProjectAndEngine(rest);
      const Uuid id = ParseUuid(rest[0]);
      const auto manifest = engine.LoadManifest(id);
      if (manifest) {
        std::cout << spatial::engine::ToJson(*manifest) << "\n";
        return 0;
      }
      const auto tasks = engine.LoadJobTasks(id);
      if (tasks.empty()) {
        std::cerr << "no such run: " << spatial::core::FormatUuid(id) << "\n";
        return 1;
      }
      for (const auto& task : tasks) {
        std::cout << spatial::core::FormatUuid(task.id) << " "
                  << task.definition.type << " "
                  << spatial::engine::TaskStatusToString(task.state) << "\n";
      }
      return 0;
    }

    if (args[0] == "report") {
      if (args.size() < 2) {
        std::cerr << kUsage;
        return 2;
      }
      std::vector<std::string> rest(args.begin() + 1, args.end());
      Engine engine = OpenProjectAndEngine(rest);
      const Uuid id = ParseUuid(rest[0]);
      return ReportCommand(id, engine);
    }

    std::cerr << kUsage;
    return 2;
  } catch (const ProjectError& e) {
    PrintError(e);
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "error: unknown non-std exception\n";
    return 1;
  }
}
