#pragma once

// COLMAP ProcessingAdapter (RFC-0008 §5/§9; C1 plan §1.1 colmap_adapter.h;
// adding-adapter.md steps 1–6). Capability declaration
// {feature_extraction, sparse_reconstruction, bundle_adjustment}, the
// configuration surface, and the CLI orchestration plan.
//
// The backend is launched, never linked: ValidateEnvironment probes the
// external `colmap` executable; there is no COLMAP build or link dependency
// anywhere in this tree (THIRD_PARTY.yml keeps COLMAP status `planned` until
// the worker proves the chain end-to-end, RFC-0008 §16).
//
// C1-S2 scope: this increment activates the seam — descriptor, environment
// probe, plan construction from the configuration model, and the Execute
// contract. Actual COLMAP CLI invocation (feature_extractor / matcher /
// mapper execution) and the process worker land in the next increment; Execute
// currently validates the plan and reports deterministic progress without
// running the backend. The engine never sees this type: it consumes only
// WorkerExecutor (ADR-034).

#include <string>
#include <vector>

#include "adapters/colmap/colmap_config.h"
#include "adapters/interfaces/processing_adapter.h"
#include "colmap_adapter_build_info.h"
#include "engine/resources/resource_spec.h"

namespace spatial::adapters::colmap {

class ColmapAdapter : public spatial::adapters::ProcessingAdapter {
 public:
  // `executable` is the COLMAP binary to probe (default "colmap"); it must be
  // discoverable on PATH or an absolute path. Replacing it is how the doctor
  // step is tested (runnable vs missing) without a COLMAP install.
  explicit ColmapAdapter(std::string executable = "colmap");

  spatial::adapters::AdapterDescriptor Descriptor() const override;
  bool ValidateEnvironment(std::string& problem) const override;
  std::vector<std::string> CreatePlan(
      const spatial::engine::TaskRequest& request) const override;
  void Execute(const std::vector<std::string>& plan,
               spatial::adapters::ResultSink& sink) override;

 private:
  std::string executable_;
  spatial::engine::ResourceProfile profile_;
};

}  // namespace spatial::adapters::colmap
