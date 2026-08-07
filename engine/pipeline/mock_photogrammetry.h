#pragma once

// The P1.4 demo pipeline (RFC-0003 P1.4): Images -> Mock Feature Extraction ->
// Mock Reconstruction -> Mock Validation (RFC-0005 QA stage). The validation
// stage runs the deterministic quality engine and produces a quality_report
// artifact. The demo implementation lives in
// engine/workers/mock_pipeline_runner; this helper only declares the
// definition and registers it in a registry.

#include "engine/pipeline/pipeline_definition.h"
#include "engine/pipeline/pipeline_registry.h"

namespace spatial::engine {

inline constexpr const char* kMockPhotogrammetryPipelineId = "photogrammetry";

// Registers the demo photogrammetry pipeline in `registry` (id
// "photogrammetry"). Safe to call multiple times on different registries.
void RegisterMockPhotogrammetry(PipelineRegistry& registry);

}  // namespace spatial::engine
