#pragma once

// AdapterDescriptor (RFC-0007 §7 placeholder activated by RFC-0008 §5; C1
// plan §3.1). The static identity and capability declaration of one adapter.
// The engine never consumes this type directly: the adapter is consumed
// inside its worker process and the engine consumes only WorkerExecutor
// (ADR-034). Capability strings come from the frozen
// worker-capabilities.schema.json taxonomy (selection by capability, never by
// vendor); license_ref resolves against THIRD_PARTY.yml (adding-adapter.md
// Step 4).
//
// This header is Constitution-protected (adapters/interfaces/**, CONSTITUTION
// §2). It depends only on std types plus the engine's header-only protocol
// value types (ResourceProfile / TaskRequest); it never links the engine.

#include <string>
#include <vector>

#include "engine/resources/resource_spec.h"

namespace spatial::adapters {

// Static identity + capability declaration of an adapter.
struct AdapterDescriptor {
  std::string adapter_id;    // stable machine-readable id, e.g. "colmap"
  std::string version;
  std::string git_commit;
  std::vector<std::string> capabilities;  // worker-capabilities.schema.json
  std::string license_ref;                // THIRD_PARTY.yml key
  spatial::engine::ResourceProfile profile;  // CPU-first; CUDA optional
  std::vector<std::string> input_artifact_kinds;
  std::vector<std::string> output_artifact_kinds;
};

}  // namespace spatial::adapters
