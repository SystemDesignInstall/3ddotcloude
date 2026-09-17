// P3-Production-E2E: Production pipeline registrations (§3.1, §3.2, §3.4).
//
// Each pipeline definition captures the stage topology for the production
// orchestration chain. The definitions are registered in the PipelineRegistry
// and discoverable through the standard engine mechanism.

#include "engine/pipeline/production_pipelines.h"

#include <utility>

#include "engine_build_info.h"

namespace spatial::engine {

void RegisterLoopClosureVerificationPipeline(PipelineRegistry& registry) {
  PipelineDefinition def;
  def.id = kLoopClosureVerificationPipelineId;
  def.name = "Loop Closure Verification";
  def.version = "0.1.0";
  def.git_commit = kEngineGitCommit;
  def.config_schema_json = "{}";
  def.stages = {
      {"verify_loop_closure", "loop_closure_verification",
       "verify_loop_closure",
       {"loop_closure_candidate", "feature", "calibration"},
       {"loop_closure"}},
  };
  registry.Register(std::move(def));
}

void RegisterLoopClosureOptimizationPipeline(PipelineRegistry& registry) {
  PipelineDefinition def;
  def.id = kLoopClosureOptimizationPipelineId;
  def.name = "Loop Closure Optimization";
  def.version = "0.1.0";
  def.git_commit = kEngineGitCommit;
  def.config_schema_json = "{}";
  def.stages = {
      {"optimize_loop_closure", "loop_closure_optimization",
       "optimize_loop_closure",
       {"loop_closure", "trajectory"},
       {"optimization_result"}},
  };
  registry.Register(std::move(def));
}

void RegisterBundleAdjustmentPipeline(PipelineRegistry& registry) {
  PipelineDefinition def;
  def.id = kBundleAdjustmentPipelineId;
  def.name = "Bundle Adjustment";
  def.version = "0.1.0";
  def.git_commit = kEngineGitCommit;
  def.config_schema_json = "{}";
  def.stages = {
      {"bundle_adjust", "bundle_adjustment", "bundle_adjust",
       {"reconstruction", "observations"},
       {"reconstruction"}},
  };
  registry.Register(std::move(def));
}

void RegisterSparseCorrectionPipeline(PipelineRegistry& registry) {
  PipelineDefinition def;
  def.id = kSparseCorrectionPipelineId;
  def.name = "Sparse Correction";
  def.version = "0.1.0";
  def.git_commit = kEngineGitCommit;
  def.config_schema_json = "{}";
  def.stages = {
      // Stage 1: worker-compute sparse reconstruction (colmap_worker behind
      // ProcessExecutor, images mode) or the canonical reconstruction
      // passthrough (reconstruction mode) — deterministic and replayable.
      {"sparse_reconstruct", "sparse_reconstruction",
       "sparse_reconstruction",
       {"image", "reconstruction"},
       {"reconstruction"}},
      // Stage 2: host COMMIT + stage-6 bundle adjustment. Reuses the existing
      // "bundle_adjustment" capability (the taxonomy is RFC-gated, P3.1 §4 P-1);
      // the in-process runner dispatches it by task_type. The stage is
      // DB-committing, so never cacheable: a replay would skip the writes.
      {"correct", "bundle_adjustment", "sparse_correction",
       {"reconstruction"},
       {"reconstruction"}},
  };
  def.stages[1].cache = CachePolicy::kNever;
  registry.Register(std::move(def));
}

void RegisterProductionPipelines(PipelineRegistry& registry) {
  RegisterLoopClosureVerificationPipeline(registry);
  RegisterLoopClosureOptimizationPipeline(registry);
  RegisterBundleAdjustmentPipeline(registry);
  RegisterSparseCorrectionPipeline(registry);
}

}  // namespace spatial::engine
