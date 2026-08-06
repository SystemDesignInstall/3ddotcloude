#pragma once

// Demo in-process implementation of pipeline tasks (RFC-0003 §5.7, ADR-021).
// Deterministic payload -> a real CAS artifact (registered in the artifact
// store) -> kArtifactProduced + kCompleted. Registering the payload in CAS is
// what lets the ADR-020 task cache serve replays (AC-8: nothing recomputed).
// This is a demo path, not the production execution path (ADR-011).

#include "core/artifacts/artifact_store.h"
#include "engine/resources/resource_spec.h"
#include "engine/workers/in_process_executor.h"

namespace spatial::engine {

// The worker profile of the demo implementation: advertises the pipeline
// capabilities (feature_extraction / reconstruction / validation) so the
// PipelineCompiler binds stages to it.
ResourceProfile DemoWorkerProfile();

// Builds a task runner that materializes a deterministic payload into CAS.
// `store` must outlive the runner.
InProcessTaskRunner MakeMockPipelineRunner(spatial::core::ArtifactStore& store);

}  // namespace spatial::engine
