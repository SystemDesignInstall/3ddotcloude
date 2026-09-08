#include "adapters/colmap/colmap_bundle_adjustment_adapter.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adapters/colmap/colmap_cli.h"
#include "adapters/colmap/colmap_converter.h"
#include "adapters/colmap/colmap_model_writer.h"
#include "adapters/process/process_runner.h"
#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"

namespace spatial::adapters::colmap {

namespace {

using spatial::core::AdapterError;
using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::Reconstruction;
using spatial::core::Sha256Hex;
using spatial::core::ValidationError;
using spatial::core::fs::CreateDirectories;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::BundleAdjustmentTrace;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::EvaluateReprojection;
using spatial::core::geometry::InitializeReprojectionViews;
using spatial::core::geometry::ReconstructionPoints;
using spatial::core::geometry::ReprojectionMetrics;
using spatial::adapters::process::ProcessOutcome;
using spatial::adapters::process::ProcessResult;
using spatial::adapters::process::ProcessSpec;
using spatial::adapters::process::RunSubprocess;

std::string FormatDouble(double value) {
  std::ostringstream out;
  out.precision(17);
  out << value;
  return out.str();
}

std::string StderrExcerpt(const ProcessResult& result) {
  if (result.stderr_text.empty()) {
    return "";
  }
  const std::string& t = result.stderr_text;
  const std::string head =
      t.size() > 400 ? "..." + t.substr(t.size() - 400) : t;
  return "\nstderr: " + head;
}

// Populates provenance for the v4 document (P13): backend identity of the
// bundle-adjuster layer, effective-configuration hash, and the inherited
// input-artifact chain + the v3 reconstruction id (sorted, no self-reference).
ReconstructionProvenanceInfo BuildProvenanceInfo(const Reconstruction& source,
                                                 const ColmapConfig& config) {
  const std::string config_hash = Sha256Hex(config.ToJson());
  ReconstructionProvenanceInfo info;
  info.backend_name = "spatial_bundle_adjuster";
  info.backend_version = config_hash;                            // effective config, deterministic
  info.adapter_version = kColmapAdapterVersion;
  info.configuration_hash = config_hash;
  info.input_artifact_hashes = source.provenance.input_artifact_hashes;
  bool have_self = false;
  for (const std::string& h : info.input_artifact_hashes) {
    if (h == source.reconstruction_id) have_self = true;
  }
  if (!have_self) {
    info.input_artifact_hashes.push_back(source.reconstruction_id);
  }
  std::sort(info.input_artifact_hashes.begin(), info.input_artifact_hashes.end());
  if (!source.provenance.engine_version.empty()) {
    info.engine_version = source.provenance.engine_version;
  }
  if (!source.provenance.engine_commit.empty()) {
    info.engine_commit = source.provenance.engine_commit;
  }
  if (!source.provenance.git_commit.empty()) {
    info.git_commit = source.provenance.git_commit;
  }
  // Timing stays zero (deterministic; the provenance policy of
  // reconstruction_feedback.h:163 / triangulation.h:490).
  info.started_at_ns = 0;
  info.finished_at_ns = 0;
  info.duration_ns = 0;
  return info;
}

}  // namespace

ColmapBundleAdjustmentAdapter::ColmapBundleAdjustmentAdapter(
    std::string executable, ColmapConfig config,
    BundleAdjusterKeypointSource keypoint_source,
    std::filesystem::path workspace, std::int64_t timeout_ms)
    : executable_(std::move(executable)),
      config_(std::move(config)),
      keypoint_source_(std::move(keypoint_source)),
      workspace_(std::move(workspace)),
      timeout_ms_(timeout_ms) {
  if (workspace_.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "bundle adjustment requires an isolated workspace directory", {},
        /*recoverable=*/false,
        "Provide a per-run workspace path (the seam seeds sparse/0 there and "
        "runs bundle_adjuster with it as the working directory).");
  }
  // Fixed-intrinsics invariant (D3/D-8c-3): defensively reject any config in
  // which an intrinsics-refine toggle is ON even though FromJson should have
  // already rejected it (P5/P12 no-partial-results).
  const auto& ba = config_.bundle_adjuster;
  if (ba.refine_focal_length || ba.refine_principal_point ||
      ba.refine_extra_params || ba.refine_intrinsics) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "bundle_adjuster config refines intrinsics; 8c keeps intrinsics FIXED "
        "(D3)",
        {}, /*recoverable=*/false,
        "Set every intrinsics-refine toggle (refine_focal_length / "
        "refine_principal_point / refine_extra_params / refine_intrinsics) "
        "to false.");
  }
}

spatial::core::geometry::BundleAdjustmentResult
ColmapBundleAdjustmentAdapter::optimize(const BundleAdjustmentInput& input) {
  // P9/D6: a pinned seed is mandatory. Rejected at config AND here.
  if (!input.random_seed || input.random_seed->empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "bundle_adjustment requires a pinned non-empty random_seed (8c D6/P9)",
        {}, /*recoverable=*/false,
        "Set BundleAdjustmentInput.random_seed to a non-empty deterministic "
        "seed (same input + same seed must give identical derived metrics).");
  }
  const std::string& seed = *input.random_seed;

  // D5 metrics of the v3 geometry (P10) and the robust-loss scale (P8: the D5
  // threshold unless explicitly overridden).
  const auto views_before = InitializeReprojectionViews(input.source);
  const auto points_before = ReconstructionPoints(input.source);
  const ReprojectionMetrics before =
      EvaluateReprojection(views_before, points_before, input.observations);
  const double loss_scale =
      config_.bundle_adjuster.loss_scale_px > 0.0
          ? config_.bundle_adjuster.loss_scale_px
          : before.threshold_px;

  // 1. Seed the workspace with the native input model (strict inverse of the
  //    reader) — keypoints resolved per image from the caller's frame_id ->
  //    FeatureArtifact chain (2D is never inline).
  const std::filesystem::path model_dir =
      SparseModelDir(workspace_);
  CreateDirectories(model_dir);
  std::map<std::uint32_t, ColmapKeypoints> keypoints_by_image;
  for (const spatial::core::ReconImage& img : input.source.images) {
    if (img.frame_id.empty()) {
      throw AdapterError(
          ErrorCode::kAdapterProcessFailed,
          "image " + std::to_string(img.image_id) +
              " ('" + img.name +
              "') has no frame_id; 2D keypoints for the native model cannot "
              "be resolved (reprojection.h: they are never stored inline)",
          {}, /*recoverable=*/false,
          "Ensure every ReconImage carries its frame_id so the seam can "
          "resolve the frame_id -> FeatureSet -> FeatureArtifact keypoints.");
    }
    std::vector<std::array<double, 2>> pixels;
    try {
      pixels = keypoint_source_(img.frame_id);
    } catch (const spatial::core::ProjectError&) {
      throw;
    } catch (const std::exception& e) {
      throw AdapterError(
          ErrorCode::kAdapterProcessFailed,
          "keypoint resolution failed for image " + std::to_string(img.image_id) +
              ": " + e.what(),
          {}, /*recoverable=*/false,
          "The frame_id -> FeatureArtifact keypoint source must return the "
          "full keypoint list for every image.");
    }
    if (pixels.empty()) {
      throw AdapterError(
          ErrorCode::kAdapterProcessFailed,
          "empty keypoint list for image " + std::to_string(img.image_id),
          {}, /*recoverable=*/false,
          "Every image in the bundle-adjustment document needs its keypoints "
          "to form the native images.bin.");
    }
    keypoints_by_image.emplace(img.image_id, std::move(pixels));
  }
  WriteColmapModel(model_dir, input.source, keypoints_by_image);

  // 2. Compose the subcommand: paths from the CLI builder, algorithm tokens
  //    from the config (incl. the fixed-intrinsics pins), then the runtime-
  //    exact loss scale + the pinned seed.
  std::vector<std::string> argv = BuildStageCommand(
      executable_, ColmapStage::kBundleAdjuster, workspace_, config_);
  argv.push_back("--BundleAdjustment.robust_loss_scale");
  argv.push_back(FormatDouble(loss_scale));
  if (!config_.seed.empty()) {
    argv.push_back("--random_seed");
    argv.push_back(config_.seed);
  }

  ProcessSpec spec;
  spec.argv = std::move(argv);
  spec.working_directory = workspace_;
  const ProcessResult result = RunSubprocess(spec, timeout_ms_, nullptr);
  if (result.outcome != ProcessOutcome::kCompleted) {
    const std::string detail =
        result.outcome == ProcessOutcome::kTimedOut
            ? "bundle_adjuster timed out"
            : result.outcome == ProcessOutcome::kCancelled
                  ? "bundle_adjuster cancelled"
                  : "bundle_adjuster could not be started";
    throw AdapterError(ErrorCode::kAdapterProcessFailed,
                       detail + ": " + result.error_message, {},
                       /*recoverable=*/false,
                       "Re-run the bundle-adjustment stage; 8c never consumes "
                       "a partial or interrupted backend result (P12-iv).");
  }
  if (result.exit_code != 0) {
    throw AdapterError(ErrorCode::kAdapterProcessFailed,
                       "bundle_adjuster exited with code " +
                           std::to_string(result.exit_code) +
                           StderrExcerpt(result),
                       {}, /*recoverable=*/false,
                       "Inspect the bundle_adjuster output; a non-zero exit "
                       "emits no v4 document (P12-iv).");
  }

  // 3. Discover + parse the refined native model (fail-closed on incomplete
  //    output) and rebuild the canonical v4 document.
  const std::vector<std::filesystem::path> files =
      DiscoverBundleAdjustmentModelFiles(workspace_);
  if (files.empty()) {
    throw AdapterError(ErrorCode::kAdapterProcessFailed,
                       "bundle_adjuster produced no output model under " +
                           (workspace_ / "sparse_ba").string(),
                       {}, /*recoverable=*/false,
                       "A bundle_adjuster run that writes no model is treated "
                       "as failed; no v4 is produced (P12-iii).");
  }
  const ReconstructionProvenanceInfo prov_info =
      BuildProvenanceInfo(input.source, config_);

  // frame_id recovery: the canonical doc pairs each image with its frame via
  // frame_id; the native model carries only names. Round the names back.
  std::map<std::string, std::string> frame_id_map;
  for (const spatial::core::ReconImage& img : input.source.images) {
    frame_id_map.emplace(img.name, img.frame_id);
  }

  Reconstruction v4 = SparseModelToReconstruction(
      ParseSparseModel(files[0], files[1], files[2]),
      FormatUuid(GenerateUuid()), input.source.scene_id,
      input.source.session_ids, input.source.coordinate_frame, prov_info,
      frame_id_map);
  v4.status = "succeeded";
  v4.created_at_ns = 0;
  // Fixed intrinsics (P5/D-8c-3): the v4 document carries the v3 intrinsics
  // verbatim — reconstruction-to-reconstruction byte-identical.
  v4.cameras = input.source.cameras;

  // 4. D5 metrics of the v4 geometry over the SAME observation set (P10/P7).
  const auto views_after = InitializeReprojectionViews(v4);
  const auto points_after = ReconstructionPoints(v4);
  const ReprojectionMetrics after =
      EvaluateReprojection(views_after, points_after, input.observations);

  // 5. Trace + backend-specific stats (P1/P13).
  BundleAdjustmentTrace trace;
  trace.converged =
      result.outcome == ProcessOutcome::kCompleted && result.exit_code == 0 &&
      std::isfinite(after.rmse_px);
  trace.iterations = config_.bundle_adjuster.max_num_iterations;
  trace.rms_before_px = before.rmse_px;
  trace.rms_after_px = after.rmse_px;
  trace.mean_before_px = before.mean_error_px;
  trace.mean_after_px = after.mean_error_px;
  trace.inlier_count_before = before.inlier_count;
  trace.outlier_count_before = before.outlier_count;
  trace.inlier_count_after = after.inlier_count;
  trace.outlier_count_after = after.outlier_count;
  trace.threshold_px_before = before.threshold_px;
  trace.threshold_px_after = after.threshold_px;

  nlohmann::json stats;
  stats["optimizer"] = config_.bundle_adjuster.loss_function;
  stats["seed"] = seed;
  stats["rms_before"] = trace.rms_before_px;
  stats["rms_after"] = trace.rms_after_px;
  stats["inlier_count_before"] = trace.inlier_count_before;
  stats["outlier_count_before"] = trace.outlier_count_before;
  stats["inlier_count_after"] = trace.inlier_count_after;
  stats["outlier_count_after"] = trace.outlier_count_after;
  stats["iterations"] = trace.iterations;
  v4.provenance.backend_specific_json = stats.dump();

  return {std::move(v4), std::move(trace)};
}

}  // namespace spatial::adapters::colmap