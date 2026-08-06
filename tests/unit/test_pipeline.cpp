// PipelineCompiler + task serialization tests (RFC-0003 P1.4, ADR-026).
// PipelineCompiler is the ONLY TaskGraph builder: these tests pin the
// identity-hash chain (pipeline hash / task hash / placeholder refs) and the
// JSON DAG round-trip used by `spatial run --dag`.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/errors/project_error.h"
#include "core/utils/sha256.h"
#include "engine/pipeline/pipeline_compiler.h"
#include "engine/pipeline/pipeline_definition.h"
#include "engine/pipeline/pipeline_registry.h"
#include "engine/task/task_serialization.h"
#include "engine/workers/mock_pipeline_runner.h"

namespace spatial::engine {
namespace {

using spatial::core::Sha256Hex;
using spatial::core::ValidationError;

PipelineDefinition MakePhotogrammetry() {
  PipelineDefinition def;
  def.id = "photogrammetry";
  def.name = "Mock Photogrammetry";
  def.version = "0.1.0";
  def.git_commit = "test-git-commit";
  def.config_schema_json = "{}";
  def.stages = {
      {"feature_extract", "feature_extraction", "feature_extract",
       {"image"}, {"keypoints"}},
      {"reconstruct", "reconstruction", "reconstruct",
       {"keypoints"}, {"point_cloud"}},
      {"validate", "validation", "validate",
       {"point_cloud"}, {"quality_report"}},
  };
  return def;
}

class PipelineCompilerTest : public ::testing::Test {
 protected:
  PipelineCompiler compiler_{"0.1.0-test", "test-git-commit"};
};

TEST_F(PipelineCompilerTest, CompilesSequentialPlanWithStableHashes) {
  const std::vector<ArtifactRef> inputs = {
      Sha256Hex(std::string("image-a")), Sha256Hex(std::string("image-b"))};
  const ExecutionPlan plan =
      compiler_.Compile(MakePhotogrammetry(), inputs, "{}",
                        DemoWorkerProfile(), "inprocess");

  ASSERT_NE(plan.job_id, Uuid{});
  ASSERT_NE(plan.graph, nullptr);
  EXPECT_EQ(plan.pipeline_id, "photogrammetry");
  EXPECT_EQ(plan.external_inputs, inputs);
  EXPECT_EQ(plan.stages.size(), 3u);
  ASSERT_EQ(plan.graph->TaskIds().size(), 3u);

  // Sequential chain: stage i depends on stage i-1; the first stage consumes
  // the real external inputs and the later ones the stable placeholders.
  EXPECT_EQ(plan.stages[0].implementation, "inprocess");
  const auto& first = plan.graph->GetTask(plan.stages[0].task_id);
  EXPECT_EQ(first.inputs, inputs);
  EXPECT_EQ(first.outputs, std::vector<ArtifactRef>{"photogrammetry/feature_extract/output"});
  EXPECT_EQ(plan.graph->GetTask(plan.stages[1].task_id).inputs,
            std::vector<ArtifactRef>{"photogrammetry/feature_extract/output"});
  EXPECT_EQ(plan.graph->GetTask(plan.stages[2].task_id).inputs,
            std::vector<ArtifactRef>{"photogrammetry/reconstruct/output"});

  const auto deps = plan.graph->DependenciesOf(plan.stages[2].task_id);
  ASSERT_EQ(deps.size(), 1u);
  EXPECT_EQ(deps[0], plan.stages[1].task_id);

  // Identity hashes: every stage carries a task hash (ADR-020 cache key).
  for (const auto& stage : plan.stages) {
    EXPECT_EQ(stage.task_hash.size(), 64u);
    EXPECT_EQ(stage.config_hash.size(), 64u);
    EXPECT_FALSE(stage.capability.empty());
  }
}

TEST_F(PipelineCompilerTest, HashChainIsDeterministicAcrossRuns) {
  const std::vector<ArtifactRef> inputs = {Sha256Hex(std::string("image-a"))};
  const std::string config = R"({"quality":"high"})";

  const auto first = compiler_.Compile(MakePhotogrammetry(), inputs, config,
                                       DemoWorkerProfile(), "inprocess");
  const auto second = compiler_.Compile(MakePhotogrammetry(), inputs, config,
                                        DemoWorkerProfile(), "inprocess");

  EXPECT_EQ(first.pipeline_hash, second.pipeline_hash);
  for (std::size_t i = 0; i < first.stages.size(); ++i) {
    EXPECT_EQ(first.stages[i].task_hash, second.stages[i].task_hash);
    EXPECT_EQ(first.stages[i].config_hash, second.stages[i].config_hash);
  }
}

TEST_F(PipelineCompilerTest, PipelineHashChangesWhenInputsChange) {
  const auto a = PipelineCompiler::PipelineHash(
      MakePhotogrammetry(), PipelineCompiler::ConfigHash("{}"),
      {Sha256Hex(std::string("x"))});
  const auto b = PipelineCompiler::PipelineHash(
      MakePhotogrammetry(), PipelineCompiler::ConfigHash("{}"),
      {Sha256Hex(std::string("y"))});
  EXPECT_NE(a, b);
}

TEST_F(PipelineCompilerTest, CompileRejectsEmptyPipeline) {
  auto def = MakePhotogrammetry();
  def.stages.clear();
  EXPECT_THROW(compiler_.Compile(def, {Sha256Hex("x")}, "{}",
                                 DemoWorkerProfile(), "inprocess"),
               ValidationError);
}

TEST_F(PipelineCompilerTest, CompileRejectsEmptyExternalInputs) {
  EXPECT_THROW(compiler_.Compile(MakePhotogrammetry(), {}, "{}",
                                 DemoWorkerProfile(), "inprocess"),
               ValidationError);
}

TEST_F(PipelineCompilerTest, CompileRejectsUnknownCapability) {
  auto def = MakePhotogrammetry();
  def.stages.push_back({"mesh", "surface_meshing", "mesh",
                        {"point_cloud"}, {"mesh"}});
  try {
    compiler_.Compile(def, {Sha256Hex("x")}, "{}", DemoWorkerProfile(),
                      "inprocess");
    FAIL() << "expected ValidationError";
  } catch (const ValidationError& e) {
    EXPECT_NE(std::string(e.what()).find("no worker advertises capability"),
              std::string::npos);
  }
}

TEST_F(PipelineCompilerTest, CompileRejectsInvalidConfigJson) {
  EXPECT_THROW(compiler_.Compile(MakePhotogrammetry(), {Sha256Hex("x")},
                                 "{not json", DemoWorkerProfile(), "inprocess"),
               ValidationError);
}

TEST_F(PipelineCompilerTest, TaskGraphJsonRoundTrip) {
  const std::vector<ArtifactRef> inputs = {Sha256Hex(std::string("image-a"))};
  const auto plan = compiler_.Compile(MakePhotogrammetry(), inputs, "{}",
                                      DemoWorkerProfile(), "inprocess");

  const std::string text = TaskGraphToJson(*plan.graph, plan.external_inputs);
  std::vector<ArtifactRef> round_inputs;
  auto graph = TaskGraphFromJson(text, round_inputs);

  EXPECT_EQ(round_inputs, inputs);
  EXPECT_EQ(graph.job_id(), plan.job_id);
  EXPECT_EQ(graph.TaskIds().size(), 3u);
  for (const auto& stage : plan.stages) {
    EXPECT_EQ(graph.GetTask(stage.task_id).definition.type,
              plan.graph->GetTask(stage.task_id).definition.type);
    const auto deps = graph.DependenciesOf(stage.task_id);
    const auto plan_deps = plan.graph->DependenciesOf(stage.task_id);
    ASSERT_EQ(deps.size(), plan_deps.size());
    for (std::size_t i = 0; i < deps.size(); ++i) {
      EXPECT_EQ(deps[i], plan_deps[i]);
    }
  }
}

}  // namespace
}  // namespace spatial::engine
