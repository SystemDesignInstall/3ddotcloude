#pragma once

// The P1.4 demo pipeline (RFC-0003 P1.4): Images -> Mock Feature Extraction ->
// Mock Reconstruction -> Mock Validation (noop). The validation stage exists
// as the reproducibility seam: today it is a noop, tomorrow it hosts the
// Accuracy/AI gates (RFC-0004). The demo implementation lives in
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
