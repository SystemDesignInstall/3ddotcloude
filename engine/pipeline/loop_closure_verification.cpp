#include "engine/pipeline/loop_closure_verification.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_manifest.h"
#include "core/errors/project_error.h"
#include "core/loop_closure/correspondence_reconstruction.h"
#include "core/loop_closure/loop_closure_candidate_gen.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/pose_graph_helpers.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine_build_info.h"

namespace spatial::engine {

using spatial::core::ArtifactManifest;
using spatial::core::ErrorCode;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::LoopClosure;
using spatial::core::LoopClosureRow;
using spatial::core::MatchingFrameDescriptors;
using spatial::core::ParseUuid;
using spatial::core::Uuid;
using nlohmann::json;

namespace {

// Serializes a canonical LoopClosure to the loop-closure.schema.json
// `LoopClosure` definition. Provenance is deliberately NOT emitted: that
// definition uses additionalProperties:false and carries no provenance field;
// provenance lives on the artifact manifest instead (P3-impl-7b §11).
json ClosureJson(const LoopClosure& lc) {
  return json{{"closure_id", lc.closure_id},
              {"trajectory_id", lc.trajectory_id},
              {"candidate_id", lc.candidate_id},
              {"source_frame_id", lc.source_frame_id},
              {"target_frame_id", lc.target_frame_id},
              {"status", lc.status},
              {"inlier_ratio", lc.inlier_ratio},
              {"inlier_count", static_cast<std::int64_t>(lc.inlier_count)},
              {"confidence", lc.confidence},
              {"temporal_separation_ns", lc.temporal_separation_ns},
              {"spatial_separation_m", lc.spatial_separation_m},
              {"has_relative_pose", lc.has_relative_pose},
              {"relative_position_xyz", lc.relative_position_xyz},
              {"relative_rotation_xyzw", lc.relative_rotation_xyzw},
              {"geometric_residual", lc.geometric_residual},
              {"created_at_ns", lc.created_at_ns}};
}

}  // namespace

LoopClosureVerificationResult VerifyLoopClosureGeometry(
    spatial::core::ArtifactStore& store, spatial::core::MetadataDb& db,
    const spatial::core::LoopClosureCandidate& candidate,
    const VerificationFrameInput& source, const VerificationFrameInput& target,
    const spatial::core::LoopClosureFeatureMatcher& matcher,
    const spatial::core::GeometricVerifier& verifier,
    const spatial::core::GeometricVerificationOptions& options,
    const std::string& configuration_hash,
    const std::optional<MetricResolutionInput>& metric_resolution) {
  LoopClosureVerificationResult result;

  // Resolve both FeatureArtifacts (keypoints + descriptors) from the CAS.
  const auto src_loaded = spatial::core::LoadFeatureDescriptors(
      store, source.feature_artifact_uuid, FormatUuid(source.frame_id),
      source.timestamp_ns);
  const auto tgt_loaded = spatial::core::LoadFeatureDescriptors(
      store, target.feature_artifact_uuid, FormatUuid(target.frame_id),
      target.timestamp_ns);
  if (!src_loaded || !tgt_loaded) {
    // Missing feature artifact -> verification FAILURE (P3-impl-7b §12, §13-B):
    // not a geometry rejection, a typed input error. Fail closed.
    throw spatial::core::ProjectError(
        ErrorCode::kValidationDomain,
        "loop closure verification: missing feature artifact");
  }
  result.artifacts_resolved = true;
  MatchingFrameDescriptors src = std::move(*src_loaded);
  MatchingFrameDescriptors tgt = std::move(*tgt_loaded);

  // Reconstruct image-space correspondences via the matcher seam (core logic).
  const auto correspondences =
      spatial::core::ReconstructCorrespondences(src, tgt, matcher);

  // Build the verifier input and run the backend-independent geometric
  // verifier (adapter supplies the concrete F + RANSAC implementation).
  spatial::core::LoopClosureVerificationInput vinput;
  vinput.candidate = candidate;
  vinput.source = std::move(src);
  vinput.target = std::move(tgt);
  vinput.correspondences = std::move(correspondences);
  // P3.1 Step 3: per-frame calibration for calibrated (essential) providers.
  // Nullopt cameras reach the verifier as-is (fundamental ignores them; a
  // calibrated provider fails closed on absence).
  vinput.source_camera = source.camera;
  vinput.target_camera = target.camera;
  result.geo = verifier.Verify(vinput, options);

  // Stamp instance identity (D-DI-01) + provenance on the canonical record.
  result.closure = result.geo.closure;
  result.closure.closure_id = FormatUuid(GenerateUuid());
  result.closure.created_at_ns =
      spatial::core::fs::TimestampNsNow();
  result.closure.provenance.configuration_hash = configuration_hash;

  // Forward the verifier's resolved metric relative pose (D5) onto the
  // canonical LoopClosure. Present ONLY when the calibrated/essential path
  // produced one (has_relative_pose); the fields are not zero-filled when
  // absent, so a verified-visual-only closure stays non-metric (INV-1).
  result.closure.has_relative_pose = result.geo.has_relative_pose;
  result.closure.relative_position_xyz = result.geo.relative_position_xyz;
  result.closure.relative_rotation_xyzw = result.geo.relative_rotation_xyzw;
  result.closure.geometric_residual = result.geo.geometric_residual;

  // P3.1 Step 3 — unit-to-metric resolution (THE call site). Only when ALL
  // hold: trajectory context supplied, trajectory metric-eligible (INV-3
  // provenance gate FIRST — the frozen resolver is basis-unaware by design,
  // so eligibility is enforced here, never inside it), closure accepted, and
  // the verifier produced a UNIT pose. The FROZEN D6 resolver converts the
  // unit direction to metres against the trajectory; only its success
  // promotes the closure (has_relative_pose=true + metric fields). `geo` is
  // deliberately left as the verifier produced it (geo.has_relative_pose
  // stays false on the unit path): estimation evidence vs orchestrated
  // resolution remain distinguishable in the return value. Every other
  // outcome stays verified-visual-only: persistable, no metric edge.
  if (result.geo.verified && result.geo.unit_relative_pose.has_value() &&
      metric_resolution.has_value() &&
      metric_resolution->trajectory_nodes != nullptr &&
      spatial::core::MetricEligibleTrajectoryBasis(
          metric_resolution->metric_basis)) {
    const spatial::core::UnitRelativePose& unit =
        *result.geo.unit_relative_pose;
    const std::optional<spatial::core::MetricLoopClosureMeasurement>
        measurement =
            spatial::core::ResolveMetricTranslationFromUnitDirection(
                result.geo.closure, *metric_resolution->trajectory_nodes,
                unit.translation_direction_xyz, unit.rotation_xyzw,
                result.geo.geometric_residual);
    if (measurement.has_value()) {
      result.closure.has_relative_pose = true;
      result.closure.relative_position_xyz = measurement->position_cs;
      result.closure.relative_rotation_xyzw = measurement->rotation_cst;
      result.closure.geometric_residual = measurement->geometric_residual;
    }
  }

  // Persist the closure (accepted OR rejected) to the loop_closures table.
  LoopClosureRow row;
  row.closure_id = ParseUuid(result.closure.closure_id);
  row.trajectory_id = ParseUuid(result.closure.trajectory_id);
  row.candidate_id = ParseUuid(result.closure.candidate_id);
  row.source_frame_id = ParseUuid(result.closure.source_frame_id);
  row.target_frame_id = ParseUuid(result.closure.target_frame_id);
  row.status = result.closure.status;
  row.inlier_ratio = result.closure.inlier_ratio;
  row.inlier_count = result.closure.inlier_count;
  row.confidence = result.closure.confidence;
  row.temporal_separation_ns = result.closure.temporal_separation_ns;
  row.spatial_separation_m = result.closure.spatial_separation_m;
  row.created_at_ns = result.closure.created_at_ns;
  db.AddLoopClosure(row);

  // Canonical loop-closure CAS payload (loop-closure.schema.json): the
  // verified candidate + its closure result.
  json candidates = json::array();
  candidates.push_back(
      json{{"candidate_id", candidate.candidate_id},
           {"trajectory_id", candidate.trajectory_id},
           {"source_frame_id", candidate.source_frame_id},
           {"target_frame_id", candidate.target_frame_id},
           {"feature_match_score", candidate.feature_match_score},
           {"matcher", candidate.matcher},
           {"created_at_ns", candidate.created_at_ns}});
  json closures = json::array();
  closures.push_back(ClosureJson(result.closure));
  const json payload_json = {
      {"schema_version", 1},
      {"candidates", candidates},
      {"closures", closures},
  };
  const std::string payload_text = payload_json.dump();
  const std::vector<std::uint8_t> payload(payload_text.begin(),
                                          payload_text.end());

  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "loop_closure";
  manifest.schema_version = 1;
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.configuration_hash = configuration_hash;
  manifest.coordinate_frame = "image";
  manifest.unit = "matches";
  manifest.mime_type = "application/json";
  const auto src_manifest = store.ReadManifest(source.feature_artifact_uuid);
  const auto tgt_manifest = store.ReadManifest(target.feature_artifact_uuid);
  if (src_manifest) manifest.input_artifact_hashes.push_back(
      src_manifest->content_hash);
  if (tgt_manifest) manifest.input_artifact_hashes.push_back(
      tgt_manifest->content_hash);

  const auto written = store.Put(payload, manifest);
  result.payload_content_hash = written.content_hash;
  result.payload_artifact_uuid = written.artifact_uuid;

  return result;
}

}  // namespace spatial::engine
