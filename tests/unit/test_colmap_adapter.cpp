// COLMAP adapter seam tests (C1-S2, RFC-0008 §5; plan §11 test_colmap_adapter).
//
// Proves: the descriptor declares exactly the ratified capability set
// {feature_extraction, sparse_reconstruction, bundle_adjustment} and rejects
// everything else (adding-adapter.md Step 8), the license reference resolves
// against THIRD_PARTY.yml, the doctor step (ValidateEnvironment) distinguishes
// a runnable backend from a missing one (Step 5), CreatePlan builds the stage
// plan from the configuration model, and the config-surface rejection
// contract holds (a calibration value in config_json is rejected, RFC-0009
// §6).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "adapters/colmap/colmap_adapter.h"
#include "adapters/colmap/colmap_config.h"
#include "adapters/interfaces/processing_adapter.h"
#include "core/errors/project_error.h"

namespace spatial::adapters::colmap {
namespace {

using spatial::core::ProjectError;
using spatial::core::ValidationError;

#ifndef SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE
#error SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE must be defined by the test build
#endif

// Records what the adapter emits, standing in for the worker-protocol bridge
// (TaskProgress / TaskLog / TaskArtifactProduced).
class RecordingSink : public spatial::adapters::ResultSink {
 public:
  void Progress(int percent, const std::string& stage) override {
    stages_.push_back(stage);
    percents_.push_back(percent);
  }
  void Log(const std::string& message) override { logs_.push_back(message); }
  void ArtifactProduced(const std::string& payload_path,
                        const std::string& manifest_json) override {
    (void)manifest_json;
    artifacts_.push_back(payload_path);
  }

  std::vector<std::string> stages_;
  std::vector<int> percents_;
  std::vector<std::string> logs_;
  std::vector<std::string> artifacts_;
};

spatial::engine::TaskRequest RequestWithConfig(const std::string& config_json) {
  spatial::engine::TaskRequest request;
  request.config_json = config_json;
  return request;
}

TEST(ColmapAdapterTest, DescriptorDeclaresExactCapabilities) {
  ColmapAdapter adapter;
  const spatial::adapters::AdapterDescriptor descriptor = adapter.Descriptor();

  EXPECT_EQ(descriptor.adapter_id, "colmap");
  EXPECT_EQ(descriptor.version, kColmapAdapterVersion);
  EXPECT_EQ(descriptor.git_commit, kColmapAdapterGitCommit);
  EXPECT_EQ(descriptor.license_ref, "COLMAP");  // THIRD_PARTY.yml key

  const std::vector<std::string> kExpected = {
      "feature_extraction", "sparse_reconstruction", "bundle_adjustment"};
  EXPECT_EQ(descriptor.capabilities, kExpected);
  EXPECT_EQ(descriptor.profile.capabilities, kExpected);
  EXPECT_EQ(descriptor.input_artifact_kinds,
            (std::vector<std::string>{"image", "calibration"}));
  EXPECT_EQ(descriptor.output_artifact_kinds,
            (std::vector<std::string>{"feature", "sparse_model"}));
}

TEST(ColmapAdapterTest, CapabilityNegotiationRejectsUnsupported) {
  // Adding-adapter.md Step 8: the engine selects adapters by capability; the
  // adapter offers exactly its declared set and nothing else.
  ColmapAdapter adapter;
  const std::vector<std::string> caps = adapter.Descriptor().capabilities;
  const auto offers = [&caps](const std::string& c) {
    return std::find(caps.begin(), caps.end(), c) != caps.end();
  };
  EXPECT_TRUE(offers("feature_extraction"));
  EXPECT_TRUE(offers("sparse_reconstruction"));
  EXPECT_TRUE(offers("bundle_adjustment"));
  EXPECT_FALSE(offers("dense_stereo"));
  EXPECT_FALSE(offers("icp"));
  EXPECT_FALSE(offers("surface_reconstruction"));
}

TEST(ColmapAdapterTest, ValidateEnvironmentSucceedsForRunnableExecutable) {
  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE);
  std::string problem;
  EXPECT_TRUE(adapter.ValidateEnvironment(problem)) << problem;
  EXPECT_TRUE(problem.empty());
}

TEST(ColmapAdapterTest, ValidateEnvironmentFailsForMissingExecutable) {
  ColmapAdapter adapter("spatial-colmap-probe-does-not-exist-xyz");
  std::string problem;
  EXPECT_FALSE(adapter.ValidateEnvironment(problem));
  EXPECT_FALSE(problem.empty());
}

TEST(ColmapAdapterTest, CreatePlanFromEmptyConfigUsesDefaultStages) {
  ColmapAdapter adapter;
  const std::vector<std::string> plan =
      adapter.CreatePlan(RequestWithConfig(""));
  EXPECT_EQ(plan, (std::vector<std::string>{
                      "feature_extractor", "matcher", "mapper"}));
}

TEST(ColmapAdapterTest, CreatePlanHonorsEnabledStages) {
  ColmapAdapter adapter;
  const std::vector<std::string> plan = adapter.CreatePlan(RequestWithConfig(
      R"({"enabled_stages": ["feature_extractor", "mapper"]})"));
  EXPECT_EQ(plan, (std::vector<std::string>{"feature_extractor", "mapper"}));
}

TEST(ColmapAdapterTest, CreatePlanCanonicalizesStageOrder) {
  // The plan always follows the canonical dependency order regardless of the
  // order the configuration lists the stages in.
  ColmapAdapter adapter;
  const std::vector<std::string> plan = adapter.CreatePlan(RequestWithConfig(
      R"({"enabled_stages": ["mapper", "feature_extractor", "matcher"]})"));
  EXPECT_EQ(plan, (std::vector<std::string>{
                      "feature_extractor", "matcher", "mapper"}));
}

TEST(ColmapAdapterTest, CreatePlanRejectsCalibrationInConfig) {
  // RFC-0009 §6: a calibration value in the configuration surface is a
  // contract violation rejected by validation.
  ColmapAdapter adapter;
  EXPECT_THROW(adapter.CreatePlan(RequestWithConfig(
                   R"({"fx": 2457.4, "fy": 2456.9, "cx": 2000.0, "cy": 1500.0})")),
               ValidationError);
}

TEST(ColmapAdapterTest, CreatePlanRejectsMalformedJson) {
  ColmapAdapter adapter;
  EXPECT_THROW(adapter.CreatePlan(RequestWithConfig("{not json")),
               ValidationError);
}

TEST(ColmapAdapterTest, CreatePlanRejectsUnknownKey) {
  ColmapAdapter adapter;
  EXPECT_THROW(adapter.CreatePlan(RequestWithConfig(R"({"bogus": 1})")),
               ValidationError);
}

TEST(ColmapAdapterTest, CreatePlanRejectsUnknownStage) {
  ColmapAdapter adapter;
  EXPECT_THROW(adapter.CreatePlan(
                   RequestWithConfig(R"({"enabled_stages": ["meshing"]})")),
               ValidationError);
}

TEST(ColmapAdapterTest, ConfigRoundTripsThroughJson) {
  const ColmapConfig config = ColmapConfig::Default();
  EXPECT_EQ(config, ColmapConfig::FromJson(config.ToJson()));
}

TEST(ColmapAdapterTest, ConfigParsesAlgorithmSettings) {
  const ColmapConfig config = ColmapConfig::FromJson(R"({
    "threads": 8,
    "seed": "20260813",
    "feature_extractor": {"max_image_size": 1600, "max_num_features": 4096},
    "matcher": {"max_ratio": 0.75},
    "mapper": {"min_num_matches": 30}
  })");
  EXPECT_EQ(config.threads, 8);
  EXPECT_EQ(config.seed, "20260813");
  EXPECT_EQ(config.feature_extractor.max_image_size, 1600);
  EXPECT_EQ(config.feature_extractor.max_num_features, 4096);
  EXPECT_EQ(config.matcher.max_ratio, 0.75);
  EXPECT_EQ(config.mapper.min_num_matches, 30);
}

TEST(ColmapAdapterTest, ConfigMarshalsStageArgs) {
  ColmapConfig config = ColmapConfig::Default();
  config.feature_extractor.max_num_features = 4096;

  const std::vector<std::string> args =
      config.BuildStageArgs(ColmapStage::kFeatureExtractor);
  EXPECT_EQ(args.size(), 12);
  EXPECT_EQ(args[0], "--SiftExtraction.max_image_size");
  EXPECT_EQ(args[1], "3200");
  EXPECT_EQ(args[2], "--SiftExtraction.max_num_features");
  EXPECT_EQ(args[3], "4096");

  const std::vector<std::string> matcher_args =
      config.BuildStageArgs(ColmapStage::kMatcher);
  EXPECT_EQ(matcher_args.size(), 8);
  EXPECT_EQ(matcher_args[0], "--SiftMatching.guided_matching");

  const std::vector<std::string> mapper_args =
      config.BuildStageArgs(ColmapStage::kMapper);
  EXPECT_EQ(mapper_args.size(), 6);
  EXPECT_EQ(mapper_args[0], "--Mapper.min_num_matches");
}

TEST(ColmapAdapterTest, ExecuteReportsDeterministicProgressPerStage) {
  // C1-S3: Execute now runs the stages as real subprocesses (the probe shim)
  // in an isolated workspace. Progress stays the deterministic 33/66/100 and
  // the mapper produces the sparse-model payload artifact.
  const std::filesystem::path workspace =
      std::filesystem::temp_directory_path() /
      ("spatial_colmap_exec_" + std::to_string(std::time(nullptr)) + "_" +
       std::to_string(rand()));
  std::filesystem::create_directories(workspace);

  auto context = std::make_shared<ExecutionContext>();
  context->workspace = workspace;

  ColmapAdapter adapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, context);
  const std::vector<std::string> plan = {"feature_extractor", "matcher",
                                         "mapper"};
  RecordingSink sink;
  adapter.Execute(plan, sink);

  EXPECT_EQ(sink.stages_, plan);
  EXPECT_EQ(sink.percents_, (std::vector<int>{33, 66, 100}));
  EXPECT_EQ(sink.artifacts_.size(), 1u);
  EXPECT_TRUE(std::filesystem::exists(sink.artifacts_.front()));

  std::error_code ec;
  std::filesystem::remove_all(workspace, ec);
}

TEST(ColmapAdapterTest, ExecuteRejectsUnknownPlanStep) {
  ColmapAdapter adapter;
  RecordingSink sink;
  EXPECT_THROW(adapter.Execute({"feature_extractor", "meshing"}, sink),
               ProjectError);
}

}  // namespace
}  // namespace spatial::adapters::colmap
