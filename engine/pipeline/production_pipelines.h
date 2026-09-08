#pragma once

// P3-Production-E2E: Production pipeline registrations for the sparse
// reconstruction correction chain. These define the conceptual flow of
// the production orchestration through the PipelineRegistry, proving the
// architecture is reachable through the standard registration/discovery
// mechanism (§3.1, §3.2, §3.4, §3.5).
//
// The actual host-side orchestration runs through dedicated functions
// (LoopClosureOptimizePipeline, BundleAdjustmentOptimizePipeline) that
// are invoked by the production entry point. The pipeline definitions
// capture the stage topology for registry/discovery and documentation.

#include "engine/pipeline/pipeline_registry.h"

namespace spatial::engine {

inline constexpr const char* kLoopClosureVerificationPipelineId =
    "loop_closure_verification";
inline constexpr const char* kLoopClosureOptimizationPipelineId =
    "loop_closure_optimization";
inline constexpr const char* kBundleAdjustmentPipelineId =
    "bundle_adjustment";
inline constexpr const char* kSparseCorrectionPipelineId =
    "p3_sparse_correction";

// Registers the Loop Closure Verification production pipeline (§3.1).
// Stage topology: {loop_closure_candidate, feature, calibration}
//   -> geometric verification -> {loop_closure}.
void RegisterLoopClosureVerificationPipeline(PipelineRegistry& registry);

// Registers the Loop Closure Optimization production pipeline (§3.2).
// Stage topology: {loop_closure, trajectory}
//   -> metric edge builder -> PoseGraph assembly -> TrajectoryOptimizer seam
//   -> {optimization_result}.
void RegisterLoopClosureOptimizationPipeline(PipelineRegistry& registry);

// Registers the Bundle Adjustment production pipeline (§3.4).
// Stage topology: {reconstruction, observations}
//   -> ReconstructionOptimizer seam -> quality gate
//   -> {reconstruction}.
void RegisterBundleAdjustmentPipeline(PipelineRegistry& registry);

// Registers all three production pipelines (§3.5 convenience).
void RegisterProductionPipelines(PipelineRegistry& registry);

}  // namespace spatial::engine
