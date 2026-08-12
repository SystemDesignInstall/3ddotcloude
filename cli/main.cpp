// spatial - the Core Platform CLI (RFC-0003 P1.4, AC-7). A thin client of the
// Engine composition root: it opens a project, registers input artifacts in
// CAS, and drives Engine::RunPipeline / RunGraph. Command surface:
//
//   spatial run <pipeline-id> [--config <json>] [--input <file> ...] [--project <dir>]
//   spatial run --dag <dag.json> [--project <dir>]
//   spatial run feature-extraction --session <uuid> [--config <json>] [--project <dir>]
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
#include "core/scene/query/scene_query.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/engine.h"
#include "engine/pipeline/feature_extraction.h"
#include "engine/pipeline/mock_photogrammetry.h"
#include "engine/pipeline/quality/quality_report.h"
#include "engine/task/task_serialization.h"
#include "importers/images/image_importer.h"

#include <nlohmann/json.hpp>

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
using spatial::importers::ImageImporter;
using spatial::importers::ImageImporterConfig;
using spatial::importers::ImageSourceFile;

constexpr const char* kUsage =
    "usage:\n"
    "  spatial init <name.spx> [--project <dir>]\n"
    "  spatial run <pipeline-id> [--config <json>] [--input <file> ...]"
    " [--project <dir>]\n"
    "  spatial run --dag <dag.json> [--project <dir>]\n"
    "  spatial run feature-extraction --session <uuid> [--config <json>]"
    " [--project <dir>]\n"
    "  spatial status <run-id> [--project <dir>]\n"
    "  spatial report <run-id> [--project <dir>]\n"
    "  spatial import <file> ... [--sensor <uuid>] [--time <ns>]"
    " [--session <uuid>] [--batch <name>] [--project <dir>]\n";

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

// Consumes exactly one flag value. Unlike CollectValues this never swallows a
// following positional argument, so it is used where positionals may follow a
// single-value flag (spatial import).
std::string SingleValue(const std::vector<std::string>& args, std::size_t& i) {
  if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) {
    throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                       "missing value for " + args[i]);
  }
  return args[++i];
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

// `spatial import` (RFC-0006 P2.1): ingests image files into the project's
// permanent Scene. Positional arguments are files; capture context comes from
// flags applied batch-wide (a single sensor/time per batch). Per-file failures
// are collected and never abort the batch (ADR-014); the result JSON is
// printed to stdout and a non-zero exit is returned iff any file failed.
int ImportCommand(const std::vector<std::string>& args, Engine& engine) {
  std::vector<ImageSourceFile> files;
  Uuid sensor_id{};
  std::int64_t timestamp_ns = 0;
  std::optional<Uuid> session;
  std::string batch_name = "import";

  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i].rfind("--", 0) != 0) {
      ImageSourceFile file;
      file.path = args[i];
      file.source_uri = args[i];
      files.push_back(std::move(file));
    } else if (args[i] == "--sensor") {
      sensor_id = ParseUuid(SingleValue(args, i));
    } else if (args[i] == "--time") {
      const std::string value = SingleValue(args, i);
      try {
        timestamp_ns = std::stoll(value);
      } catch (const std::exception&) {
        throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                           "--time expects a decimal nanoseconds value: " +
                               value);
      }
    } else if (args[i] == "--session") {
      session = ParseUuid(SingleValue(args, i));
    } else if (args[i] == "--batch") {
      batch_name = SingleValue(args, i);
    } else {
      throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                         "unknown argument: " + args[i]);
    }
  }

  for (auto& file : files) {
    file.sensor_id = sensor_id;
    file.timestamp_ns = timestamp_ns;
  }

  ImageImporterConfig config;
  config.batch_name = batch_name;
  ImageImporter importer(engine.project().artifacts(), engine.project().db(),
                         engine.project().info().uuid, config);

  const auto result = importer.Import(files, session);

  nlohmann::json j;
  j["session_id"] = spatial::core::FormatUuid(result.session_id);
  j["imported"] = nlohmann::json::array();
  for (const auto& e : result.imported) {
    j["imported"].push_back(
        {{"frame_id", spatial::core::FormatUuid(e.frame_id)},
         {"observation_id", spatial::core::FormatUuid(e.observation_id)},
         {"artifact_uuid", spatial::core::FormatUuid(e.artifact_uuid)},
         {"content_hash", e.content_hash},
         {"mime_type", e.mime_type},
         {"width", e.width},
         {"height", e.height},
         {"pixel_format", e.pixel_format},
         {"reimported", e.reimported},
         {"new_instance", e.new_instance},
         {"uncalibrated", e.uncalibrated}});
  }
  j["failures"] = nlohmann::json::array();
  for (const auto& f : result.failures) {
    j["failures"].push_back(
        {{"source_uri", f.source_uri},
         {"code", spatial::core::StableErrorCode(f.code)},
         {"diagnostic", f.diagnostic}});
  }
  std::cout << j.dump(2) << "\n";

  return result.failures.empty() ? 0 : 1;
}

// `spatial run feature-extraction --session <uuid>` (P2.3, RFC-0007 §8): the
// session/CLI layer resolves each observation's image to its content hash
// (SceneQuery::ArtifactHash), runs the single-stage feature_extraction
// pipeline per image, and records the per-frame feature_sets row via the
// canonical scene-aware writer. The worker stays scene- and DB-free
// (ADR-038); this command is the sole production writer of feature_sets.
// Idempotent re-execution (AC-8): identical inputs replay from the ADR-020
// task cache and DeriveFeatureSetId re-records nothing (deduplicated=true).
// Per-observation failures are collected and never abort the batch
// (ADR-014); non-zero exit iff any observation failed.
int FeatureExtractionSessionCommand(const std::vector<std::string>& args,
                                    Engine& engine) {
  Uuid session_id{};
  std::string config;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--session") {
      session_id = ParseUuid(SingleValue(args, i));
    } else if (args[i] == "--config") {
      config = CollectValues(args, i).front();
    } else {
      throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                         "unknown argument: " + args[i]);
    }
  }
  if (session_id == Uuid{}) {
    throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                       "feature-extraction requires --session <uuid>");
  }

  spatial::core::SceneQuery query(engine.project().db());
  if (!query.FindCaptureSession(session_id)) {
    throw ProjectError(spatial::core::ErrorCode::kValidationDomain,
                       "no such session: " +
                           spatial::core::FormatUuid(session_id));
  }
  const auto observations = query.ObservationsBySession(session_id);

  nlohmann::json out;
  out["session_id"] = spatial::core::FormatUuid(session_id);
  out["feature_sets"] = nlohmann::json::array();
  out["failures"] = nlohmann::json::array();

  for (const auto& observation : observations) {
    const auto hash = query.ArtifactHash(observation.artifact_ref);
    if (!hash) {
      out["failures"].push_back(
          {{"frame_id", spatial::core::FormatUuid(observation.frame_id)},
           {"code", "no_artifact_hash"},
           {"diagnostic", "observation artifact is not in the project"}});
      continue;
    }
    const auto manifest = engine.RunPipeline(
        spatial::engine::kFeatureExtractionPipelineId, {*hash}, config);
    if (manifest.status != "succeeded" || manifest.stages.empty() ||
        manifest.stages.front().output_refs.empty()) {
      out["failures"].push_back(
          {{"frame_id", spatial::core::FormatUuid(observation.frame_id)},
           {"code", "pipeline_failed"},
           {"diagnostic", manifest.status}});
      continue;
    }
    const std::string feature_hash =
        manifest.stages.front().output_refs.front();
    const auto payload = engine.project().artifacts().Get(feature_hash);
    if (!payload) {
      out["failures"].push_back(
          {{"frame_id", spatial::core::FormatUuid(observation.frame_id)},
           {"code", "feature_payload_missing"},
           {"diagnostic", feature_hash}});
      continue;
    }
    const std::string text(payload->begin(), payload->end());
    const auto payload_json = nlohmann::json::parse(text);

    // Provenance (P2.3 M1): copy the effective stage configuration hash the
    // worker recorded on the FeatureArtifact manifest so the session-layer
    // artifact instance carries identical provenance; the CAS re-Put dedups
    // to the same payload bytes either way.
    std::string configuration_hash;
    const auto worker_artifact =
        engine.project().db().FindArtifactByHash(feature_hash);
    if (worker_artifact) {
      const auto worker_manifest = engine.project().artifacts().ReadManifest(
          worker_artifact->artifact_id);
      if (worker_manifest) {
        configuration_hash = worker_manifest->configuration_hash;
      }
    }

    spatial::engine::WriteFeatureArtifactInput input;
    input.frame_id = observation.frame_id;
    input.detector = payload_json.at("detector").get<std::string>();
    input.descriptor_type =
        payload_json.at("descriptor_type").get<std::string>();
    input.input_content_hash = *hash;
    input.configuration_hash = configuration_hash;
    for (const auto& kp : payload_json.at("keypoints")) {
      spatial::engine::FeaturePoint p;
      p.x = kp.at("x").get<double>();
      p.y = kp.at("y").get<double>();
      p.size = kp.value("size", 0.0);
      p.angle = kp.value("angle", 0.0);
      p.response = kp.value("response", 0.0);
      input.keypoints.push_back(p);
    }
    for (const auto& row : payload_json.at("descriptors")) {
      input.descriptors.push_back(row.get<std::vector<double>>());
    }

    const auto result = spatial::engine::WriteFeatureArtifact(
        engine.project().artifacts(), engine.project().db(), input);

    out["feature_sets"].push_back(
        {{"frame_id", spatial::core::FormatUuid(result.feature_set.frame_id)},
         {"feature_set_id",
          spatial::core::FormatUuid(result.feature_set.feature_set_id)},
         {"detector", result.feature_set.detector},
         {"descriptor_type", result.feature_set.descriptor_type},
         {"count", result.feature_set.count},
         {"artifact_ref",
          spatial::core::FormatUuid(result.feature_set.artifact_ref)},
         {"content_hash", result.content_hash},
         {"deduplicated", result.deduplicated}});
  }

  std::cout << out.dump(2) << "\n";
  return out["failures"].empty() ? 0 : 1;
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
      RegisterFeatureExtraction(engine.registry());
      const std::string pipeline_id = rest[0];
      std::vector<std::string> flags(rest.begin() + 1, rest.end());
      // Session-scoped feature extraction is its own command (RFC-0007 §8):
      // the session layer resolves observations and owns feature_sets. The
      // hyphenated form is the CLI name; the pipeline id (underscore) remains
      // the engine identity used by RunPipeline.
      if (pipeline_id == "feature-extraction") {
        return FeatureExtractionSessionCommand(flags, engine);
      }
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

    if (args[0] == "import") {
      if (args.size() < 2) {
        std::cerr << kUsage;
        return 2;
      }
      std::vector<std::string> rest(args.begin() + 1, args.end());
      Engine engine = OpenProjectAndEngine(rest);
      return ImportCommand(rest, engine);
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
