#pragma once

// COLMAP-backed implementation of the canonical Bundle Adjustment seam
// (P3-impl-8c P2/P15/P16; docs/architecture/P3-impl-8c-bundle-adjustment-
// readiness.md §5). Runs the external `colmap bundle_adjuster` subcommand
// behind core::geometry::ReconstructionOptimizer; the engine and the tests
// reach COLMAP ONLY through this class — neither engine/pipeline nor the
// seamed call sites include any COLMAP type (P2, engine rule).
//
// COLMAP stays LAUNCHED, never linked (colmap_adapter.h:8-12, RFC-0008 §16):
// this file composes argv via colmap_cli.BuildStageCommand and drives the
// child through adapters/process. The only genuinely-new COLMAP code this
// increment adds over the C1 base is the native-model WRITER
// (colmap_model_writer), which materializes the canonical v3 into
// sparse/0/cameras.bin+images.bin+points3D.bin so bundle_adjuster can run.
//
// Fixed-intrinsics policy (D3/D-8c-3): intrinsics are constants — the v3
// ReconCamera values are written to the native model verbatim and copied back
// onto the v4 document unchanged. The COLMAP refine flags are pinned OFF by
// ColmapConfig::FromJson (P5/P16); this class additionally fails closed if a
// config somehow slips through with an intrinsics-refine toggle ON.
//
// The robust-loss scale (P8/D-8c-4) is the D5 threshold of the v3 observation
// set unless config.bundle_adjuster.loss_scale_px > 0 (auto rule);
// deterministic for equal inputs.
//
// Fail-closed (P12): missing/unpinned seed, unresolved keypoints, unsupported
// camera model, non-finite geometry, subprocess non-zero exit or timeout,
// incomplete/absent native output, or an observation set whose references do
// not resolve all throw a typed ProjectError (ValidationError /
// AdapterError, ADR-014) and nothing is emitted.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "adapters/colmap/colmap_config.h"
#include "colmap_adapter_build_info.h"
#include "core/geometry/reconstruction_optimizer.h"

namespace spatial::adapters::colmap {

// Resolves the FULL 2D keypoint list (pixels, point2d_idx index space) for
// one frame_id — the frame_id -> FeatureSet -> FeatureArtifact chain the
// caller owns (2D is never inline in the reconstruction document,
// reprojection.h). An empty result or a std::exception fails the seam closed.
using BundleAdjusterKeypointSource = std::function<std::vector<std::array<double, 2>>(
    const std::string& frame_id)>;

class ColmapBundleAdjustmentAdapter
    : public spatial::core::geometry::ReconstructionOptimizer {
 public:
  // `executable` is the COLMAP binary (default "colmap"; the probe shim in
  // tests). `config` must already carry a pinned non-empty seed for a BA run
  // (enforced at FromJson AND here). `keypoint_source` resolves frame_id ->
  // keypoints for the native images.bin (see WriteColmapModel). `workspace`
  // is the isolated per-run directory (empty fails closed): the seam seeds
  // sparse/0 with the writer, runs bundle_adjuster with that working
  // directory, and reads the refined model back from sparse_ba.
  explicit ColmapBundleAdjustmentAdapter(
      std::string executable, ColmapConfig config,
      BundleAdjusterKeypointSource keypoint_source,
      std::filesystem::path workspace = {},
      std::int64_t timeout_ms = 600000);

  spatial::core::geometry::BundleAdjustmentResult optimize(
      const spatial::core::geometry::BundleAdjustmentInput& input) override;

 private:
  std::string executable_;
  ColmapConfig config_;
  BundleAdjusterKeypointSource keypoint_source_;
  std::filesystem::path workspace_;
  std::int64_t timeout_ms_;
};

}  // namespace spatial::adapters::colmap