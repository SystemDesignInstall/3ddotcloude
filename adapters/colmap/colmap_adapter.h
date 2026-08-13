#pragma once

// COLMAP ProcessingAdapter (RFC-0008 §5/§9; C1 plan §1.1 colmap_adapter.h;
// adding-adapter.md steps 1–6). Capability declaration
// {feature_extraction, sparse_reconstruction, bundle_adjustment}, the
// configuration surface, and the CLI orchestration plan.
//
// The backend is launched, never linked: the adapter runs the external
// `colmap` executable through the generic subprocess runner
// (adapters/process/process_runner.h); there is no COLMAP build or link
// dependency anywhere in this tree (THIRD_PARTY.yml keeps COLMAP status
// `planned` until the worker proves the chain end-to-end, RFC-0008 §16).
//
// C1-S3 (this increment): real CLI execution. Execute() prepares the
// isolated task workspace (plan §4), materializes TaskRequest.input_refs from
// the CAS into local files (RFC-0009 §6 — the CLI never receives a content
// hash as a path), runs each stage as a real subprocess with stdout/stderr
// capture, exit-code mapping, a per-stage deadline and cooperative
// cancellation, then discovers produced outputs and emits canonical artifacts
// (payload + provenance manifest) through the ResultSink. The host verifies
// and ingests those into the CAS (C1-S1 fail-closed ingest contract); the
// adapter itself never writes CAS structures.
//
// The engine never sees this type: it consumes only WorkerExecutor (ADR-034).
// The sealed §3.1 seam (adapters/interfaces/processing_adapter.h) is
// unchanged; per-task execution context is bound to the concrete adapter via
// its constructor (the future COLMAP worker builds one instance per task).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "adapters/colmap/colmap_config.h"
#include "adapters/interfaces/processing_adapter.h"
#include "colmap_adapter_build_info.h"
#include "engine/resources/resource_spec.h"

namespace spatial::core {
class ArtifactStore;
}

namespace spatial::adapters::process {
class CancelToken;
}

namespace spatial::adapters::colmap {

// Per-task execution context the worker binds to the adapter (plan §4, §5).
// `workspace` is the deterministic temp/<job>/<task> directory; `store` is the
// project ArtifactStore used to materialize `input_refs` (CAS content hashes,
// image + optional CalibrationArtifact) into local workspace files before the
// CLI runs; `input_kinds` is parallel to `input_refs` and draws from the
// declared input_artifact_kinds {image, calibration}. `config_json` must be
// the same effective configuration CreatePlan saw (its SHA-256 becomes the
// manifest configuration_hash, RFC-0009 §6). `cancel` is the host's
// cooperative-cancellation token (worker-protocol TaskCancelled).
struct ExecutionContext {
  std::filesystem::path workspace;
  spatial::core::ArtifactStore* store = nullptr;
  std::vector<std::string> input_refs;
  std::vector<std::string> input_kinds;
  std::string config_json;
  std::int64_t stage_timeout_ms = 600000;              // per CLI-tool deadline
  spatial::adapters::process::CancelToken* cancel = nullptr;
};

class ColmapAdapter : public spatial::adapters::ProcessingAdapter {
 public:
  // `executable` is the COLMAP binary to run (default "colmap"; absolute path
  // or resolvable on PATH). Replacing it is how execution is tested without a
  // COLMAP install (the probe shim). `context` is the per-task execution
  // context; when null, Execute has no workspace/CAS and fails closed.
  explicit ColmapAdapter(
      std::string executable = "colmap",
      std::shared_ptr<const ExecutionContext> context = nullptr);

  spatial::adapters::AdapterDescriptor Descriptor() const override;
  bool ValidateEnvironment(std::string& problem) const override;
  std::vector<std::string> CreatePlan(
      const spatial::engine::TaskRequest& request) const override;
  void Execute(const std::vector<std::string>& plan,
               spatial::adapters::ResultSink& sink) override;

 private:
  // Fail-closed materialization of input_refs into <workspace>/inputs/<hash>
  // (and images/ + calibration.json staging) via CAS lookup.
  void MaterializeInputs(const std::filesystem::path& workspace);

  // Canonical provenance manifest for a discovered output payload (RFC-0008
  // §8; producer = adapter identity, input_artifact_hashes = sorted input
  // refs, configuration_hash = SHA-256 of the effective config).
  std::string BuildManifest(const std::filesystem::path& payload);

  std::string executable_;
  spatial::engine::ResourceProfile profile_;
  std::shared_ptr<const ExecutionContext> context_;
};

}  // namespace spatial::adapters::colmap
