// P3.1 host-runner for the p3_sparse_correction pipeline. See the header and
// docs/architecture/P3.1-production-sparse-correction-closure.md (§4 P-2/P-3).
//
// Stage ordering and fail-closed guarantees mirror the E2E reference chain
// (tests/unit/test_p3_production_e2e.cpp): the commit materializes the worker
// CAS payload as a SUCCEEDED revision BEFORE the stage-6 gate runs, so a
// rejected gate leaves the committed revision as the ONLY succeeded row and
// writes nothing further (P12: no half-persisted correction).

#include "engine/pipeline/sparse_correction_runner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/artifacts/artifact_manifest.h"
#include "core/errors/project_error.h"
#include "core/geometry/triangulation.h"
#include "core/geometry/triangulation_observations.h"
#include "core/loop_closure/loop_closure_candidate_gen.h"
#include "core/reconstruction/reconstruction_json.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/optimization.h"
#include "core/trajectory/optimization_result_json.h"
#include "core/trajectory/optimizer.h"
#include "core/trajectory/pose_graph_json.h"
#include "core/trajectory/reconstruction_feedback.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/bundle_adjustment_optimize_pipeline.h"
#include "engine/pipeline/loop_closure_to_pose_graph.h"
#include "engine/pipeline/loop_closure_verification.h"
#include "engine/pipeline/sparse_correction_orchestrator.h"
#include "engine/workers/mock_pipeline_runner.h"
#include "engine/workers/process_executor.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ArtifactManifest;
using spatial::core::ArtifactStore;
using spatial::core::ErrorCode;
using spatial::core::FeatureKeypoint;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::GeometricVerificationOptions;
using spatial::core::LoadFeatureDescriptors;
using spatial::core::LoopClosure;
using spatial::core::LoopClosureCandidate;
using spatial::core::OptimizationResult;
using spatial::core::OptimizationResultRow;
using spatial::core::OptimizationResultToJson;
using spatial::core::OptimizedPoseNode;
using spatial::core::PoseFeedbackDetail;
using spatial::core::PoseGraph;
using spatial::core::PoseGraphEdge;
using spatial::core::PoseGraphNode;
using spatial::core::PoseGraphRow;
using spatial::core::PoseGraphToJson;
using spatial::core::PoseOptimizationInput;
using spatial::core::PoseOptimizationOutput;
using spatial::core::ReconstructionFeedbackInput;
using spatial::core::ReconstructionFeedbackResult;
using spatial::core::ReconstructionRow;
using spatial::core::geometry::MakeTriangulationObservation;
using spatial::core::geometry::Retriangulate;
using spatial::core::geometry::RetriangulationOptions;
using spatial::core::geometry::RetriangulationResult;
using spatial::core::geometry::TriangulationObservation;
using spatial::core::MetadataDb;
using spatial::core::MetricBasis;
using spatial::core::MetricBasisSource;
using spatial::core::MetricBasisType;
using spatial::core::ParseUuid;
using spatial::core::ProjectError;
using spatial::core::ReconCamera;
using spatial::core::ReconImage;
using spatial::core::Reconstruction;
using spatial::core::ReconstructionFromJson;
using spatial::core::ReconstructionToJson;
using spatial::core::SceneRow;
using spatial::core::Trajectory;
using spatial::core::TrajectoryPoseNode;
using spatial::core::Uuid;
using spatial::core::ValidationError;
using spatial::core::fs::Iso8601UtcNow;
using nlohmann::json;

// P3.1 §2: the run-level configuration the pipeline user provides is wrapped
// into every effective stage config under the "config" member by the
// PipelineCompiler (pipeline_compiler.cpp:134-138).
json EffectiveConfig(const TaskRequest& request) {
  json parsed;
  try {
    parsed = request.config_json.empty() ? json::object()
                                         : json::parse(request.config_json);
  } catch (const json::parse_error&) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: malformed stage configuration");
  }
  return parsed.contains("config") && parsed["config"].is_object()
             ? parsed["config"]
             : json::object();
}

// The compile-time config the scheduler hands every task is the SAME run
// configuration for all stages (the compiler wraps it into each stage's
// effective config under "config"). The colmap worker owns a strict
// configuration surface (threads/seed/feature_extractor/matcher/mapper/
// bundle_adjuster/enabled_stages) and rejects unknown keys; host plumbing
// (project_id/scene_name/random_seed/mode) must never cross the worker
// boundary. This runner is the executor-boundary owner (sparse_correction
// _runner.h), so it forwards the worker ONLY its own settings, preserving the
// compiler envelope ({pipeline_id, pipeline_version, stage, config}) when
// present so the worker's unwrap-and-validate path stays intact.
json CleanseWorkerConfig(const std::string& config_json) {
  const std::unordered_set<std::string> kWorkerOwnKeys = {
      "threads", "seed", "feature_extractor", "matcher", "mapper",
      "bundle_adjuster", "enabled_stages",
  };
  json doc = json::object();
  if (!config_json.empty()) {
    try {
      doc = json::parse(config_json);
    } catch (const json::parse_error&) {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "sparse correction: malformed worker configuration");
    }
  }
  auto keep_worker_own = [&kWorkerOwnKeys](const json& in) {
    json out = json::object();
    for (auto it = in.begin(); it != in.end(); ++it) {
      if (kWorkerOwnKeys.count(it.key()) != 0) {
        out[it.key()] = it.value();
      }
    }
    return out;
  };
  if (doc.is_object() && doc.size() == 4 && doc.contains("pipeline_id") &&
      doc.contains("pipeline_version") && doc.contains("stage") &&
      doc.contains("config") && doc["config"].is_object()) {
    json out = doc;
    out["config"] = keep_worker_own(doc["config"]);
    return out;
  }
  return keep_worker_own(doc);
}

void EmitCompleted(const TaskRequest& request,
                   const std::function<void(WorkerEvent)>& emit) {
  WorkerEvent completed;
  completed.type = WorkerEventType::kCompleted;
  completed.task_id = request.task_id;
  emit(completed);
}

void EmitCancelled(const TaskRequest& request,
                   const std::function<void(WorkerEvent)>& emit) {
  WorkerEvent cancelled;
  cancelled.type = WorkerEventType::kCancelled;
  cancelled.task_id = request.task_id;
  emit(cancelled);
}

// Writes `payload` into the CAS and reports the produced + completed events in
// the uniform worker shape. `input_hashes` flows into the manifest lineage.
void EmitProduced(ArtifactStore& store, const TaskRequest& request,
                  const std::vector<std::uint8_t>& payload,
                  const std::string& artifact_type,
                  std::vector<ArtifactRef> input_hashes,
                  const std::function<void(WorkerEvent)>& emit) {
  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = artifact_type;
  manifest.schema_version = 2;  // canonical reconstruction documents (§2)
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.input_artifact_hashes = std::move(input_hashes);
  manifest.creation_timestamp = Iso8601UtcNow();
  manifest.file_size = static_cast<std::int64_t>(payload.size());
  manifest.mime_type = "application/octet-stream";

  const auto produced_hash = store.Put(payload, manifest).content_hash;

  WorkerEvent produced;
  produced.type = WorkerEventType::kArtifactProduced;
  produced.task_id = request.task_id;
  produced.artifact_ref = produced_hash;
  emit(produced);

  EmitCompleted(request, emit);
}

// ---- P3.1 Step 4: LC context resolution (production sources) ----
//
// Audit result — exact production sources, no second source of truth:
//   FrameID ........ run config loop_closure.candidate.{source,target}_frame_id
//   Observations ... CAS FeatureArtifacts via loop_closure.{source,target}_frame
//                    .feature_artifact_uuid (fallback: feature_artifacts map)
//   Reconstruction . CAS input document, parsed pre-commit (`source`)
//   ReconCamera .... source.cameras via source.images[frame_id].camera_id
//                    (D-CRM-04/05/20 chain, in-memory, no DB / SceneQuery /
//                    worker access; uses the PRE-COMMIT source so verification
//                    stays before the first correction write — P12)
//   Trajectory ..... run config metric.trajectory (nodes + metric_basis; the
//                    documented runner contract, validated — never invented)
//   LC candidate ... run config loop_closure.candidate
// A half-configured segment (only one of metric.trajectory / loop_closure)
// fails closed; an absent segment degrades to the plain commit + BA path.
// A structurally valid but metric-ineligible basis (e.g. by-fiat declared)
// is NOT an error: it yields a verified-visual-only closure (INV-3).

const json& RequireObjectMember(const json& doc, const char* key,
                                const std::string& what) {
  if (!doc.is_object() || !doc.contains(key) || !doc.at(key).is_object()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what + " requires object '" +
                              key + "'");
  }
  return doc.at(key);
}

std::string RequireString(const json& value, const std::string& what) {
  if (!value.is_string()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what + " must be a string");
  }
  return value.get<std::string>();
}

std::string RequireStringMember(const json& doc, const char* key,
                                const std::string& what) {
  if (!doc.is_object() || !doc.contains(key)) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what + " is missing '" +
                              key + "'");
  }
  return RequireString(doc.at(key), what + "." + key);
}

double RequireNumberMember(const json& doc, const char* key,
                           const std::string& what) {
  if (!doc.is_object() || !doc.contains(key) ||
      !doc.at(key).is_number()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what + " requires number '" +
                              key + "'");
  }
  const double v = doc.at(key).get<double>();
  if (!std::isfinite(v)) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what + "." + key +
                              " must be finite");
  }
  return v;
}

std::int64_t OptionalIntMember(const json& doc, const char* key,
                               const std::string& what) {
  if (!doc.is_object() || !doc.contains(key)) return 0;
  if (!doc.at(key).is_number_integer()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what + "." + key +
                              " must be an integer");
  }
  return doc.at(key).get<std::int64_t>();
}

Uuid ParseFrameUuid(const std::string& value, const std::string& what) {
  try {
    return ParseUuid(value);
  } catch (const std::exception&) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what + " ('" + value +
                              "') is not a valid UUID frame id");
  }
}

// FrameID -> ReconCamera via the canonical reconstruction document:
// frame_id -> ReconImage (by frame_id, D-CRM-20) -> camera_id -> ReconCamera
// (D-CRM-04/05). Throws when the frame is not reconstructed or its camera
// record is absent: verification failure, never a fallback calibration.
const ReconCamera& FindCameraForFrame(const Reconstruction& recon,
                                      const std::string& frame_id,
                                      const char* role) {
  const ReconImage* image = nullptr;
  for (const auto& im : recon.images) {
    if (im.frame_id == frame_id) {
      image = &im;
      break;
    }
  }
  if (image == nullptr) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        std::string("sparse correction: loop closure ") + role +
            " frame '" + frame_id + "' is not in the reconstruction");
  }
  for (const auto& cam : recon.cameras) {
    if (cam.camera_id == image->camera_id) return cam;
  }
  throw ValidationError(
      ErrorCode::kValidationDomain,
      std::string("sparse correction: loop closure ") + role + " frame '" +
          frame_id + "' references camera_id " +
          std::to_string(image->camera_id) + " with no ReconCamera record");
}

std::array<double, 3> RequirePosition3(const json& doc,
                                       const std::string& what) {
  if (!doc.is_object() || !doc.contains("position_xyz") ||
      !doc.at("position_xyz").is_array() ||
      doc.at("position_xyz").size() != 3) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what +
                              " requires position_xyz[3]");
  }
  std::array<double, 3> out{};
  for (int i = 0; i < 3; ++i) {
    const json& v = doc.at("position_xyz").at(static_cast<std::size_t>(i));
    if (!v.is_number() || !std::isfinite(v.get<double>())) {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "sparse correction: " + what +
                                " position_xyz must be finite numbers");
    }
    out[static_cast<std::size_t>(i)] = v.get<double>();
  }
  return out;
}

std::array<double, 4> RequireRotation4(const json& doc,
                                       const std::string& what) {
  if (!doc.is_object() || !doc.contains("rotation_xyzw") ||
      !doc.at("rotation_xyzw").is_array() ||
      doc.at("rotation_xyzw").size() != 4) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what +
                              " requires rotation_xyzw[4] (x,y,z,w)");
  }
  std::array<double, 4> out{};
  for (int i = 0; i < 4; ++i) {
    const json& v = doc.at("rotation_xyzw").at(static_cast<std::size_t>(i));
    if (!v.is_number() || !std::isfinite(v.get<double>())) {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "sparse correction: " + what +
                                " rotation_xyzw must be finite numbers");
    }
    out[static_cast<std::size_t>(i)] = v.get<double>();
  }
  return out;
}

std::vector<TrajectoryPoseNode> ParseTrajectoryNodes(const json& traj) {
  if (!traj.is_object() || !traj.contains("nodes") ||
      !traj.at("nodes").is_array() || traj.at("nodes").empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: metric.trajectory requires a non-empty "
        "nodes[] array (missing trajectory context fails closed)");
  }
  std::vector<TrajectoryPoseNode> nodes;
  std::size_t index = 0;
  for (const json& n : traj.at("nodes")) {
    const std::string what =
        "sparse correction: metric.trajectory.nodes[" + std::to_string(index) +
        "]";
    if (!n.is_object()) {
      throw ValidationError(ErrorCode::kValidationDomain,
                            what + " must be an object");
    }
    TrajectoryPoseNode node;
    node.frame_id = RequireStringMember(n, "frame_id", what);
    node.timestamp_ns = OptionalIntMember(n, "timestamp_ns", what);
    node.sequence_index = OptionalIntMember(n, "sequence_index", what);
    node.position_xyz = RequirePosition3(n, what);
    node.rotation_xyzw = RequireRotation4(n, what);
    nodes.push_back(std::move(node));
    ++index;
  }
  return nodes;
}

MetricBasis ParseMetricBasis(const json& traj) {
  MetricBasis basis;
  if (!traj.is_object() || !traj.contains("metric_basis")) return basis;
  const json& mb = traj.at("metric_basis");
  if (!mb.is_object()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: metric_basis must be an object");
  }
  if (!mb.contains("declared") || !mb.at("declared").is_boolean()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: metric_basis.declared must be a boolean");
  }
  basis.declared = mb.at("declared").get<bool>();
  if (mb.contains("type")) {
    const std::string type = RequireString(mb.at("type"), "metric_basis.type");
    if (type == "odometry_scale") {
      basis.basis = MetricBasisType::kOdometryScale;
    } else if (type == "calibrated_baseline") {
      basis.basis = MetricBasisType::kCalibratedBaseline;
    } else if (type == "sfm_metric_aligned") {
      basis.basis = MetricBasisType::kSfmMetricAligned;
    } else if (type == "ground_truth") {
      basis.basis = MetricBasisType::kGroundTruth;
    } else {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "sparse correction: unknown metric_basis.type '" +
                                type + "'");
    }
  }
  if (mb.contains("source")) {
    const std::string source =
        RequireString(mb.at("source"), "metric_basis.source");
    if (source == "trajectory") {
      basis.source = MetricBasisSource::kTrajectory;
    } else if (source == "reconstruction") {
      basis.source = MetricBasisSource::kReconstruction;
    } else if (source == "combined") {
      basis.source = MetricBasisSource::kCombined;
    } else {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "sparse correction: unknown metric_basis.source '" +
                                source + "'");
    }
  }
  if (mb.contains("scale_calibration_ref")) {
    basis.scale_calibration_ref =
        RequireString(mb.at("scale_calibration_ref"),
                      "metric_basis.scale_calibration_ref");
  }
  if (mb.contains("provenance") && mb.at("provenance").is_object() &&
      mb.at("provenance").contains("configuration_hash")) {
    basis.provenance.configuration_hash = RequireString(
        mb.at("provenance").at("configuration_hash"),
        "metric_basis.provenance.configuration_hash");
  }
  // NOTE: an invalid declaration (e.g. by-fiat declared=true without
  // scale_calibration_ref/provenance) is NOT a parse error: ValidateMetricBasis
  // rejects it downstream and the closure stays verified-visual-only (INV-3).
  return basis;
}

LoopClosureCandidate ParseLoopClosureCandidate(const json& lc) {
  const json& cand = RequireObjectMember(lc, "candidate", "loop_closure");
  LoopClosureCandidate out;
  out.candidate_id = RequireStringMember(cand, "candidate_id", "candidate");
  out.trajectory_id = RequireStringMember(cand, "trajectory_id", "candidate");
  out.source_frame_id =
      RequireStringMember(cand, "source_frame_id", "candidate");
  out.target_frame_id =
      RequireStringMember(cand, "target_frame_id", "candidate");
  out.feature_match_score =
      RequireNumberMember(cand, "feature_match_score", "candidate");
  out.matcher = RequireStringMember(cand, "matcher", "candidate");
  out.created_at_ns = OptionalIntMember(cand, "created_at_ns", "candidate");
  return out;
}

struct LcFrameBlock {
  Uuid frame_id{};
  std::string frame_id_string;
  std::int64_t timestamp_ns = 0;
  Uuid feature_artifact_uuid{};
};

// Frame block: {frame_id, timestamp_ns, feature_artifact_uuid}, with the
// documented feature_artifacts map as fallback for the artifact uuid only.
LcFrameBlock ParseLoopClosureFrame(const json& lc, const char* key,
                                   const json& feature_map) {
  const json& block = RequireObjectMember(lc, key, "loop_closure");
  const std::string what = std::string("loop_closure.") + key;
  LcFrameBlock out;
  out.frame_id_string = RequireStringMember(block, "frame_id", what);
  out.frame_id = ParseFrameUuid(out.frame_id_string, what + ".frame_id");
  if (!block.contains("timestamp_ns") ||
      !block.at("timestamp_ns").is_number_integer()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + what +
                              " requires integer timestamp_ns");
  }
  out.timestamp_ns = block.at("timestamp_ns").get<std::int64_t>();
  std::string artifact;
  if (block.contains("feature_artifact_uuid") &&
      block.at("feature_artifact_uuid").is_string()) {
    artifact = block.at("feature_artifact_uuid").get<std::string>();
  } else if (feature_map.is_object() &&
             feature_map.contains(out.frame_id_string) &&
             feature_map.at(out.frame_id_string).is_string()) {
    artifact = feature_map.at(out.frame_id_string).get<std::string>();
  }
  if (artifact.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: " + what +
            " has no feature_artifact_uuid (missing observation reference "
            "fails closed)");
  }
  out.feature_artifact_uuid = ParseFrameUuid(artifact, what +
      ".feature_artifact_uuid");
  return out;
}

// The verified loop-closure context handed from the Step-4 segment to the
// Step-5 graph assembly: the persisted closure (metric iff the frozen
// resolver promoted it), the resolved trajectory context, and the CAS hash
// of the loop-closure evidence payload (manifest lineage, no re-derivation).
struct VerifiedLoopClosure {
  LoopClosure closure;
  std::vector<TrajectoryPoseNode> nodes;
  MetricBasis metric_basis;
  std::string coordinate_frame;
  std::string loop_closure_payload_hash;
};

// The production LC/trajectory segment: strict production-resolved context
// into the Step-3 orchestrator. Returns nullopt when the segment was not
// requested; throws BEFORE any database write on strict failures (P12).
std::optional<VerifiedLoopClosure> MaybeRunLoopClosureVerification(
    MetadataDb& db, ArtifactStore& store, const json& cfg,
    const Reconstruction& source, const SparseCorrectionSeams& seams,
    const std::string& pipeline_hash) {
  const bool has_traj = cfg.is_object() && cfg.contains("metric") &&
                        cfg.at("metric").is_object() &&
                        cfg.at("metric").contains("trajectory");
  const bool has_lc =
      cfg.is_object() && cfg.contains("loop_closure") &&
      cfg.at("loop_closure").is_object();
  if (!has_traj && !has_lc) return std::nullopt;  // plain commit + BA path
  if (has_traj != has_lc) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: half-configured loop-closure segment: "
        "metric.trajectory and loop_closure must both be present");
  }

  // Strict parse of the documented run-config contract (structural failures
  // throw here, before any write).
  const json& traj = cfg.at("metric").at("trajectory");
  if (!traj.is_object()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: metric.trajectory must be an "
                          "object");
  }
  const std::vector<TrajectoryPoseNode> nodes = ParseTrajectoryNodes(traj);
  const MetricBasis basis = ParseMetricBasis(traj);
  const json& lc = cfg.at("loop_closure");
  const LoopClosureCandidate candidate = ParseLoopClosureCandidate(lc);
  const json feature_map =
      (cfg.contains("feature_artifacts") &&
       cfg.at("feature_artifacts").is_object())
          ? cfg.at("feature_artifacts")
          : json::object();
  const LcFrameBlock src_frame =
      ParseLoopClosureFrame(lc, "source_frame", feature_map);
  const LcFrameBlock tgt_frame =
      ParseLoopClosureFrame(lc, "target_frame", feature_map);

  // Seam dependencies (P12 extension): missing matcher/verifier fails closed
  // before any database write.
  if (seams.matcher == nullptr || seams.verifier == nullptr) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: loop-closure segment requested but the "
        "matcher/verifier seams were not injected");
  }

  // Per-frame calibration from the canonical reconstruction document itself.
  const ReconCamera& src_camera =
      FindCameraForFrame(source, src_frame.frame_id_string, "source");
  const ReconCamera& tgt_camera =
      FindCameraForFrame(source, tgt_frame.frame_id_string, "target");

  VerificationFrameInput src_in;
  src_in.frame_id = src_frame.frame_id;
  src_in.timestamp_ns = src_frame.timestamp_ns;
  src_in.feature_artifact_uuid = src_frame.feature_artifact_uuid;
  src_in.camera = src_camera;
  VerificationFrameInput tgt_in;
  tgt_in.frame_id = tgt_frame.frame_id;
  tgt_in.timestamp_ns = tgt_frame.timestamp_ns;
  tgt_in.feature_artifact_uuid = tgt_frame.feature_artifact_uuid;
  tgt_in.camera = tgt_camera;

  MetricResolutionInput metric;
  metric.trajectory_nodes = &nodes;
  metric.metric_basis = basis;

  // The production call: real cameras, real nodes, real basis — nothing here
  // is test-injected beyond the run config + CAS contents, which ARE the
  // production inputs. A rejected closure persists as rejected (7b) and the
  // stage continues to commit + BA. The verified context is returned for the
  // Step-5 graph assembly (PoseGraph); GTSAM consumption stays deferred.
  const LoopClosureVerificationResult verified = VerifyLoopClosureGeometry(
      store, db, candidate, src_in, tgt_in, *seams.matcher, *seams.verifier,
      GeometricVerificationOptions{}, pipeline_hash,
      std::optional<MetricResolutionInput>(metric));

  std::string coordinate_frame;
  if (traj.contains("coordinate_frame") &&
      traj.at("coordinate_frame").is_string()) {
    coordinate_frame = traj.at("coordinate_frame").get<std::string>();
  }
  VerifiedLoopClosure out;
  out.closure = verified.closure;
  out.nodes = std::move(nodes);
  out.metric_basis = basis;
  out.coordinate_frame = std::move(coordinate_frame);
  out.loop_closure_payload_hash = verified.payload_content_hash;
  return out;
}

// P3.1 Step 5 — measurement -> graph boundary. Builds the production
// PoseGraph from a verified loop closure through the FROZEN
// LoopClosureToPoseGraph stage (never from the unit pose, never through a
// second converter). Only an already-resolved metric closure
// (has_relative_pose=true, metres) with a metric-eligible basis can yield an
// edge — every other outcome persists nothing: no graph, no edge (INV-3).
// The graph carries the trajectory nodes as D-PG-03 graph nodes (id +
// frame_id + timestamp; pose data is NOT duplicated) plus the single
// loop-closure edge, verbatim (no rescaling). Persisted as a PoseGraphRow
// (queryable metadata) + the canonical pose-graph CAS payload (full nodes +
// edges, pose-graph.schema.json). Status "ready": built, not optimizing
// (GTSAM is a later step).
// The assembled production graph handed from Step-5 persistence to the
// Step-6 optimizer: canonical graph + nodes + the single loop edge (all
// persisted already) plus the trajectory context as optimizer initial
// values and the graph payload hash for result lineage.
struct AssembledPoseGraph {
  PoseGraph graph;
  std::vector<PoseGraphNode> graph_nodes;
  std::vector<PoseGraphEdge> graph_edges;
  std::vector<TrajectoryPoseNode> initial_nodes;
  std::string coordinate_frame;
  std::string graph_payload_hash;
};

std::optional<AssembledPoseGraph> MaybePersistLoopClosurePoseGraph(
    MetadataDb& db, ArtifactStore& store, const VerifiedLoopClosure& verified,
    const Uuid& scene_id, const std::string& pipeline_hash) {
  LoopClosureToPoseGraphInput stage_in;
  stage_in.trajectory = nullptr;  // stage resolves frames over nodes only
  stage_in.trajectory_nodes = &verified.nodes;
  stage_in.closure = verified.closure;
  stage_in.metric_basis = verified.metric_basis;
  stage_in.configuration_hash = pipeline_hash;
  const LoopClosureToPoseGraphResult stage = LoopClosureToPoseGraph(stage_in);
  if (!stage.metric_edge.has_value())
    return std::nullopt;  // visual-only/rejected: clean

  const PoseGraphEdge edge = *stage.metric_edge;
  std::vector<PoseGraphNode> graph_nodes;
  graph_nodes.reserve(verified.nodes.size());
  for (std::size_t i = 0; i < verified.nodes.size(); ++i) {
    PoseGraphNode node;
    node.node_id = static_cast<std::int64_t>(i);
    node.frame_id = verified.nodes[i].frame_id;
    node.timestamp_ns = verified.nodes[i].timestamp_ns;
    graph_nodes.push_back(std::move(node));
  }

  PoseGraph graph;
  graph.graph_id = FormatUuid(GenerateUuid());
  graph.trajectory_id = verified.closure.trajectory_id;
  graph.scene_id = FormatUuid(scene_id);
  graph.status = "ready";
  graph.node_count = static_cast<std::int64_t>(graph_nodes.size());
  graph.edge_count = 1;
  graph.odometry_edge_count = 0;
  graph.loop_closure_edge_count = 1;
  graph.prior_edge_count = 0;
  graph.created_at_ns = verified.closure.created_at_ns;
  graph.provenance.backend.name = "spatial_pose_graph_builder";
  graph.provenance.backend.version = kEngineVersion;
  graph.provenance.backend.adapter_version = kEngineVersion;
  graph.provenance.configuration_hash = pipeline_hash;
  graph.provenance.input_artifact_hashes = {
      verified.loop_closure_payload_hash};
  graph.provenance.engine_version = kEngineVersion;
  graph.provenance.engine_commit = kEngineGitCommit;

  const std::string document =
      PoseGraphToJson(graph, graph_nodes, {edge});
  PoseGraphRow row;
  row.graph_id = ParseUuid(graph.graph_id);
  row.trajectory_id = ParseUuid(graph.trajectory_id);
  row.scene_id = scene_id;
  row.status = graph.status;
  row.node_count = graph.node_count;
  row.edge_count = graph.edge_count;
  row.odometry_edge_count = graph.odometry_edge_count;
  row.loop_closure_edge_count = graph.loop_closure_edge_count;
  row.prior_edge_count = graph.prior_edge_count;
  row.created_at_ns = graph.created_at_ns;
  row.document_json = document;
  db.AddPoseGraph(row);

  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "pose_graph";
  manifest.schema_version = 1;
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.input_artifact_hashes = {verified.loop_closure_payload_hash};
  manifest.configuration_hash = pipeline_hash;
  manifest.creation_timestamp = Iso8601UtcNow();
  manifest.coordinate_frame = verified.coordinate_frame;
  manifest.unit = "meter";
  manifest.file_size =
      static_cast<std::int64_t>(document.size());
  manifest.mime_type = "application/json";
  const std::string graph_hash =
      store.Put(std::vector<std::uint8_t>(document.begin(), document.end()),
                manifest)
          .content_hash;

  AssembledPoseGraph assembled;
  assembled.graph = graph;
  assembled.graph_nodes = std::move(graph_nodes);
  assembled.graph_edges = {edge};
  assembled.initial_nodes = verified.nodes;
  assembled.coordinate_frame = verified.coordinate_frame;
  assembled.graph_payload_hash = graph_hash;
  return assembled;
}

// P3.1 Step 6 — graph -> optimizer boundary. Runs the persisted production
// graph through the injected TrajectoryOptimizer seam (D-AB-02: the engine
// never names the backend; GTSAM is only ever the optimizer here, never the
// measurement source — the measurement was closed in Steps 1-5). The seam's
// anchor prior fixes gauge on node 0; initial values are the resolved
// trajectory context. On a converged run the NEW optimized trajectory is
// persisted as an OptimizationResultRow + canonical CAS payload; the input
// closure/graph/nodes are NEVER mutated (immutability rule: the result is a
// separate entity referencing the graph). Anything else — missing seam,
// backend throw, non-converged status, size mismatch, non-finite poses,
// out-of-domain errors — fails closed with NO optimization result (no false
// optimized trajectory). No Apply here (Step 7): the result is evidence.
// The persisted Step-6 output handed to Step-7 apply: canonical result +
// corrected nodes + its CAS hash (apply lineage, no re-derivation).
struct PersistedOptimization {
  OptimizationResult result;
  std::vector<OptimizedPoseNode> optimized_nodes;
  std::string result_payload_hash;
};

PersistedOptimization MaybeRunTrajectoryOptimization(
    MetadataDb& db, ArtifactStore& store, const AssembledPoseGraph& assembled,
    const SparseCorrectionSeams& seams, const std::string& pipeline_hash) {
  // Lazy P12: the optimizer seam is required only when a graph was actually
  // assembled (no graph -> the optimizer is never needed; O7). A graph
  // without an optimizer fails the stage — silently skipping would leave a
  // "ready" graph that never optimizes.
  if (seams.trajectory_optimizer == nullptr) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: pose graph assembled but no TrajectoryOptimizer "
        "seam was injected; the optimization backend provider is unavailable");
  }

  PoseOptimizationInput input;
  input.graph = assembled.graph;
  input.graph_nodes = assembled.graph_nodes;
  input.graph_edges = assembled.graph_edges;
  input.initial_nodes = assembled.initial_nodes;
  // Seam defaults: anchor prior on node 0 (gauge), enabled.
  const PoseOptimizationOutput output =
      seams.trajectory_optimizer->optimize(input);

  // Strict output validation BEFORE any consumption (no false trajectory).
  if (output.trace.status != "converged" || !output.trace.converged) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: trajectory optimization did not converge (status '" +
            output.trace.status + "'); no optimized trajectory is produced");
  }
  if (output.optimized_nodes.size() != assembled.initial_nodes.size()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: optimizer returned a node set that does not "
        "match the trajectory context; no optimized trajectory is produced");
  }
  if (output.result_id.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: optimizer returned no result id; no optimized "
        "trajectory is produced");
  }
  for (const auto& node : output.optimized_nodes) {
    for (const double v : node.position_xyz) {
      if (!std::isfinite(v)) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: optimizer returned a non-finite position; no "
            "optimized trajectory is produced");
      }
    }
    for (const double v : node.rotation_xyzw) {
      if (!std::isfinite(v)) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: optimizer returned a non-finite rotation; no "
            "optimized trajectory is produced");
      }
    }
  }
  if (!(output.trace.initial_error >= 0.0) ||
      !(output.trace.final_error >= 0.0) ||
      !(output.trace.error_reduction >= 0.0) ||
      !(output.trace.error_reduction <= 1.0) ||
      !(output.trace.iterations >= 0)) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: optimizer trace is outside its diagnostic domain; "
        "no optimized trajectory is produced");
  }

  OptimizationResult result;
  result.result_id = output.result_id;
  result.graph_id = assembled.graph.graph_id;
  result.trajectory_id = assembled.graph.trajectory_id;
  result.status = "converged";
  result.iterations = output.trace.iterations;
  result.initial_error = output.trace.initial_error;
  result.final_error = output.trace.final_error;
  result.error_reduction = output.trace.error_reduction;
  result.created_at_ns = assembled.graph.created_at_ns;
  // Boundary honesty (D-AB-02): the engine side knows only the seam that
  // produced this result, never the backend behind it. The backend name is
  // therefore the seam identity; a future RFC may add backend-name reporting
  // to the seam itself — it is NOT smuggled around the boundary here.
  result.provenance.optimizer.name = "trajectory_optimizer";
  result.provenance.optimizer.version = "";
  result.provenance.configuration_hash = pipeline_hash;
  result.provenance.input_artifact_hashes = {assembled.graph_payload_hash};
  result.provenance.adapter_version = "";
  result.provenance.git_commit = "";

  std::vector<OptimizedPoseNode> opt_nodes;
  opt_nodes.reserve(output.optimized_nodes.size());
  for (const auto& node : output.optimized_nodes) {
    OptimizedPoseNode opt;
    opt.frame_id = node.frame_id;
    opt.timestamp_ns = node.timestamp_ns;
    opt.sequence_index = node.sequence_index;
    opt.position_xyz = node.position_xyz;
    opt.rotation_xyzw = node.rotation_xyzw;
    opt.covariance_position = node.covariance_position;
    opt.covariance_rotation = node.covariance_rotation;
    opt_nodes.push_back(std::move(opt));
  }

  const std::string document = OptimizationResultToJson(result, opt_nodes);
  OptimizationResultRow row;
  row.result_id = ParseUuid(result.result_id);
  row.graph_id = ParseUuid(result.graph_id);
  row.trajectory_id = ParseUuid(result.trajectory_id);
  row.status = result.status;
  row.iterations = result.iterations;
  row.initial_error = result.initial_error;
  row.final_error = result.final_error;
  row.error_reduction = result.error_reduction;
  row.created_at_ns = result.created_at_ns;
  row.document_json = document;
  db.AddOptimizationResult(row);

  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "optimization_result";
  manifest.schema_version = 1;
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.input_artifact_hashes = {assembled.graph_payload_hash};
  manifest.configuration_hash = pipeline_hash;
  manifest.creation_timestamp = Iso8601UtcNow();
  manifest.coordinate_frame = assembled.coordinate_frame;
  manifest.unit = "meter";
  manifest.file_size = static_cast<std::int64_t>(document.size());
  manifest.mime_type = "application/json";
  const std::string result_hash =
      store.Put(std::vector<std::uint8_t>(document.begin(), document.end()),
                manifest)
          .content_hash;

  PersistedOptimization persisted;
  persisted.result = result;
  persisted.optimized_nodes = std::move(opt_nodes);
  persisted.result_payload_hash = result_hash;
  return persisted;
}

// P3.1 Step 7 — apply boundary. Feeds the persisted optimization result back
// into a NEW canonical reconstruction revision through the FROZEN
// ApplyOptimizedTrajectory seam (poses only, SC-1): points, cameras,
// detected flags, and image order pass through verbatim — no
// re-triangulation, no BA here. The join key is frame_id string equality
// (never vector-index matching); the seam preserves unmatched images.
//
// Runner-level strictness beyond the seam contract (fail closed, no vacuous
// or dangling apply):
//   - same-frame declaration: config trajectory.coordinate_frame must equal
//     the source reconstruction's coordinate_frame (CF-1); otherwise the
//     T_reconstruction_trajectory alignment is unknown and Identity would be
//     dishonest. Cross-frame alignment resolution is deferred (no estimation
//     here) — mismatch fails closed. The seam's alignment_resolved=false path
//     stays as backstop.
//   - every optimized node must reference a source image frame (no dangling
//     measurement), all poses finite (the seam validator has a NaN hole:
//     NaN < 1e-3 is false, so finiteness is enforced here), and at least one
//     image must actually be updated (no vacuous revision).
// The new revision coexists as "succeeded" WITHOUT superseding the committed
// source: Step 8's re-triangulation supersedes the applied revision (its
// direct source) and Step 9's bundle adjustment supersedes the v3 geometry
// (its source). The committed revision is NOT a step-8/9 source once the
// corrected chain materializes v3, so Step 9 chain-compacts it to
// "superseded" before the BA stage runs (its root role is fulfilled). A
// here-and-now supersede inside Step 7 would corrupt those transitions
// (double-supersede throw). Latest-by-clock dynamics until Step 9 (BA consumes
// the applied revision) are documented: wall-clock evidence ordering,
// deterministic revision chain via created_at + provenance lineage, never
// renumbering.
// The persisted Step-7 output handed to Step-8 re-triangulation: the applied
// revision document plus its CAS payload hash (geometry lineage, no
// re-derivation).
struct AppliedRevision {
  Reconstruction reconstruction;
  std::string payload_hash;
};

AppliedRevision MaybeApplyOptimizedTrajectory(
    MetadataDb& db, ArtifactStore& store, const Reconstruction& committed,
    const VerifiedLoopClosure& verified,
    const PersistedOptimization& optimization, const Uuid& scene_id,
    const std::string& source_cas_hash, const std::string& pipeline_hash) {
  if (optimization.optimized_nodes.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: optimization produced no pose nodes; refusing a "
        "vacuous apply");
  }

  // Same-frame declaration (CF-1): the trajectory frame must name the
  // reconstruction frame, else T_reconstruction_trajectory is unknown.
  if (verified.coordinate_frame.empty() ||
      verified.coordinate_frame != committed.coordinate_frame) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: trajectory frame '" + verified.coordinate_frame +
            "' does not match reconstruction frame '" +
            committed.coordinate_frame +
            "'; cross-frame alignment is not resolved in this step");
  }

  // Frame membership + finiteness over the optimized nodes (join keys must
  // resolve into the source document; NaN must never reach the seam).
  for (const auto& node : optimization.optimized_nodes) {
    if (node.frame_id.empty()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "sparse correction: optimized node with an empty frame id cannot "
          "be applied");
    }
    bool known = false;
    for (const auto& image : committed.images) {
      if (image.frame_id == node.frame_id) {
        known = true;
        break;
      }
    }
    if (!known) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "sparse correction: optimized node references frame '" +
              node.frame_id + "' absent from the reconstruction; refusing a "
              "dangling apply");
    }
    for (const double v : node.position_xyz) {
      if (!std::isfinite(v)) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: optimized node for frame '" + node.frame_id +
                "' carries a non-finite position; refusing the apply");
      }
    }
    for (const double v : node.rotation_xyzw) {
      if (!std::isfinite(v)) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: optimized node for frame '" + node.frame_id +
                "' carries a non-finite rotation; refusing the apply");
      }
    }
  }

  Trajectory trajectory;
  trajectory.trajectory_id = verified.closure.trajectory_id;
  trajectory.coordinate_frame = verified.coordinate_frame;

  ReconstructionFeedbackInput feedback;
  feedback.source = committed;
  feedback.trajectory = trajectory;
  feedback.trajectory_nodes = verified.nodes;
  feedback.optimization_result = optimization.result;
  for (const auto& node : optimization.optimized_nodes) {
    OptimizedPoseNode opt;
    opt.frame_id = node.frame_id;
    opt.timestamp_ns = node.timestamp_ns;
    opt.sequence_index = node.sequence_index;
    opt.position_xyz = node.position_xyz;
    opt.rotation_xyzw = node.rotation_xyzw;
    opt.covariance_position = node.covariance_position;
    opt.covariance_rotation = node.covariance_rotation;
    feedback.optimized_nodes.push_back(std::move(opt));
  }
  feedback.reconstruction_from_trajectory =
      spatial::core::geometry::SE3::Identity();
  feedback.alignment_resolved = true;
  feedback.source_reconstruction_cas_hash = source_cas_hash;
  feedback.optimization_result_cas_hash = optimization.result_payload_hash;

  std::vector<PoseFeedbackDetail> details;
  ReconstructionFeedbackResult applied;
  try {
    applied = ApplyOptimizedTrajectory(feedback, &details);
  } catch (const std::invalid_argument& ex) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          std::string("sparse correction: apply rejected: ") +
                              ex.what());
  }
  if (!ValidateOptimizedReconstruction(applied.reconstruction)) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: applied reconstruction failed structural "
        "validation; refusing to persist it");
  }
  bool any_updated = false;
  for (const auto& detail : details) {
    if (detail.updated) {
      any_updated = true;
      break;
    }
  }
  if (!any_updated) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: optimization matched no reconstruction image; "
        "refusing a vacuous apply");
  }

  // Persist the new revision (append-only; committed stays succeeded so Step 9
  // can chain-compact and then hand the v3 geometry to the BA stage below).
  const std::string document =
      ReconstructionToJson(applied.reconstruction);
  ReconstructionRow row;
  row.reconstruction_id =
      ParseUuid(applied.reconstruction.reconstruction_id);
  row.scene_id = scene_id;
  row.coordinate_frame = applied.reconstruction.coordinate_frame;
  row.status = applied.reconstruction.status;
  row.created_at_ns = applied.reconstruction.created_at_ns;
  row.document_json = document;
  db.AddReconstruction(row);

  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "reconstruction";
  manifest.schema_version = 2;
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.input_artifact_hashes =
      applied.reconstruction.provenance.input_artifact_hashes;
  manifest.configuration_hash = pipeline_hash;
  manifest.creation_timestamp = Iso8601UtcNow();
  manifest.coordinate_frame = applied.reconstruction.coordinate_frame;
  manifest.unit = "meter";
  manifest.file_size = static_cast<std::int64_t>(document.size());
  manifest.mime_type = "application/octet-stream";
  const std::string applied_hash =
      store.Put(std::vector<std::uint8_t>(document.begin(), document.end()),
                manifest)
          .content_hash;
  return AppliedRevision{applied.reconstruction, applied_hash};
}

// P3.1 Step 8 — geometry recomputation boundary. Recomputes the 3D points of
// the applied revision from the CORRECTED camera poses through the FROZEN 8b
// re-triangulation (DLT closest-point + the §4.10 acceptance predicate + the
// global D5 residual gate), and persists the result as a NEW canonical
// revision. Geometry only: no BA, no pose re-optimization, no GTSAM, no
// scale/alignment applied anywhere.
//
// Observations are resolved through the canonical production chain
//   frame_id -> feature_sets (MetadataDb) -> FeatureArtifact (CAS)
//            -> keypoints[point2d_idx]
// (the same LoadFeatureDescriptors reader the loop-closure stage uses), with
// the documented run-config `feature_artifacts` map as an explicit pin. The
// runner never invents observations and never falls back to the source points:
// a point whose observations cannot be resolved fails the stage, and a source
// revision with no points at all produces no v3 (nothing to recompute).
//
// Fail closed (no v3 revision, typed error) when: a tracked frame has no
// feature set / an ambiguous one / a missing artifact; a point2d_idx is out of
// range; the frozen predicate rejects EVERY point (a correction that destroys
// all geometry is not a correction). The frozen function's own per-point
// rejection (cheirality / parallax / D5) is preserved as-is.
//
// Revision clock: the frozen Retriangulate returns created_at_ns = 0 (its
// payload-determinism rule); the runner stamps applied.created_at_ns + 100 so
// the P14 chain stays strictly increasing (same "+N extension" pattern the
// commit step uses). The applied revision is superseded by v3 in ONE
// transaction (its direct source); the committed revision is chain-compacted
// by Step 9 once v3 materializes, and the BA stage supersedes its own source.
std::optional<Reconstruction> MaybeRetriangulateAppliedRevision(
    MetadataDb& db, ArtifactStore& store, const AppliedRevision& applied,
    const Uuid& scene_id, const json& feature_pins,
    const std::string& pipeline_hash) {
  const Reconstruction& source = applied.reconstruction;
  if (source.points3D.empty()) {
    return std::nullopt;  // vacuous: no geometry to recompute (never fabricated)
  }

  // image_id -> frame_id (canonical identity for observation resolution).
  std::vector<std::pair<std::uint32_t, std::string>> frame_by_image;
  frame_by_image.reserve(source.images.size());
  for (const auto& image : source.images) {
    frame_by_image.emplace_back(image.image_id, image.frame_id);
  }

  // Resolve keypoints only for the images the tracks actually observe.
  std::vector<std::pair<std::uint32_t, std::vector<FeatureKeypoint>>> keypoints;
  std::vector<std::string> observation_hashes;
  auto resolve = [&](std::uint32_t image_id) -> const std::vector<FeatureKeypoint>& {
    for (const auto& entry : keypoints) {
      if (entry.first == image_id) return entry.second;
    }
    std::string frame_id;
    bool known = false;
    for (const auto& entry : frame_by_image) {
      if (entry.first == image_id) {
        frame_id = entry.second;
        known = true;
        break;
      }
    }
    if (!known || frame_id.empty()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "sparse correction: re-triangulation track references unknown "
          "image_id " + std::to_string(image_id));
    }
    Uuid artifact{};
    if (feature_pins.is_object() && feature_pins.contains(frame_id) &&
        feature_pins.at(frame_id).is_string()) {
      try {
        artifact = ParseUuid(feature_pins.at(frame_id).get<std::string>());
      } catch (const std::exception&) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: feature_artifacts pin for frame '" + frame_id +
                "' is not a valid artifact UUID");
      }
    } else {
      Uuid frame_uuid{};
      try {
        frame_uuid = ParseUuid(frame_id);
      } catch (const std::exception&) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: reconstruction frame_id '" + frame_id +
                "' is not a valid UUID; observations cannot be resolved");
      }
      const auto rows = db.FindFeatureSetsByFrame(frame_uuid);
      if (rows.empty()) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: frame '" + frame_id +
                "' has no feature set; re-triangulation observations cannot "
                "be resolved (fail closed, no fallback to source points)");
      }
      if (rows.size() > 1) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: frame '" + frame_id + "' has " +
                std::to_string(rows.size()) +
                " feature sets; pin one via feature_artifacts (ambiguous "
                "observation source fails closed)");
      }
      try {
        artifact = ParseUuid(rows.front().artifact_ref);
      } catch (const std::exception&) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: feature set of frame '" + frame_id +
                "' carries a malformed artifact_ref");
      }
    }
    const auto loaded = LoadFeatureDescriptors(store, artifact, frame_id, 0);
    if (!loaded) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "sparse correction: feature artifact of frame '" + frame_id +
              "' is absent from the CAS or is not a feature artifact");
    }
    if (const auto manifest = store.ReadManifest(artifact);
        manifest.has_value()) {
      observation_hashes.push_back(manifest->content_hash);
    }
    keypoints.emplace_back(image_id, loaded->keypoints);
    return keypoints.back().second;
  };

  std::vector<TriangulationObservation> observations;
  for (const auto& point : source.points3D) {
    for (const auto& element : point.track) {
      const std::vector<FeatureKeypoint>& kps = resolve(element.image_id);
      if (element.point2d_idx < 0 ||
          static_cast<std::size_t>(element.point2d_idx) >= kps.size()) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: track observation point2d_idx " +
                std::to_string(element.point2d_idx) + " of point " +
                std::to_string(point.point3d_id) +
                " is outside the resolved feature artifact");
      }
      const auto& kp =
          kps[static_cast<std::size_t>(element.point2d_idx)];
      observations.push_back(MakeTriangulationObservation(
          element.image_id, element.point2d_idx, kp.x, kp.y));
    }
  }

  // Frozen 8b re-triangulation with its documented defaults (2 deg parallax,
  // 1 cm stability). No configuration surface is invented here.
  const RetriangulationResult result =
      Retriangulate(source, observations, RetriangulationOptions{});
  if (result.reconstruction.points3D.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: re-triangulation accepted no points under the "
        "corrected poses (rejected " +
            std::to_string(result.lineage.rejected_count) +
            "); refusing to persist an empty-geometry revision");
  }

  Reconstruction v3 = result.reconstruction;
  // Revision clock (see the function comment): the frozen function returns 0.
  v3.created_at_ns = source.created_at_ns + 100;
  // Lineage evidence the frozen function leaves empty: the classification
  // summary (P3-impl-8 §4.11 pattern), deterministic and derived from the run.
  v3.provenance.backend_specific_json =
      std::string("{\"preserved\":") +
      std::to_string(result.lineage.preserved_count) + ",\"replaced\":" +
      std::to_string(result.lineage.replaced_count) + ",\"new\":" +
      std::to_string(result.lineage.new_count) + ",\"rejected\":" +
      std::to_string(result.lineage.rejected_count) + "}";

  const std::string document = ReconstructionToJson(v3);
  ReconstructionRow row;
  row.reconstruction_id = ParseUuid(v3.reconstruction_id);
  row.scene_id = scene_id;
  row.coordinate_frame = v3.coordinate_frame;
  row.status = v3.status;
  row.created_at_ns = v3.created_at_ns;
  row.document_json = document;
  {
    MetadataDb::WriteTransaction txn(db);
    db.AddReconstruction(row);
    db.SetReconstructionStatus(ParseUuid(source.reconstruction_id),
                               "superseded");
    txn.Commit();
  }

  // CAS payload lineage: the applied revision document + the resolved
  // observation artifacts (the re-triangulation inputs).
  std::vector<std::string> lineage = observation_hashes;
  lineage.push_back(applied.payload_hash);
  std::sort(lineage.begin(), lineage.end());
  lineage.erase(std::unique(lineage.begin(), lineage.end()), lineage.end());

  ArtifactManifest manifest;
  manifest.artifact_uuid = GenerateUuid();
  manifest.type = "reconstruction";
  manifest.schema_version = 2;
  manifest.producer = {"spatial-platform", kEngineVersion, kEngineGitCommit};
  manifest.input_artifact_hashes = std::move(lineage);
  manifest.configuration_hash = pipeline_hash;
  manifest.creation_timestamp = Iso8601UtcNow();
  manifest.coordinate_frame = v3.coordinate_frame;
  manifest.unit = "meter";
  manifest.file_size = static_cast<std::int64_t>(document.size());
  manifest.mime_type = "application/octet-stream";
  store.Put(std::vector<std::uint8_t>(document.begin(), document.end()),
            manifest);
  return v3;
}

// P3-impl-8c — resolves the 2D->3D observation set the bundle-adjustment seam
// consumes. The points' tracks are the authority for (image_id, point3d_id)
// membership; the keypoint pixels come from the SAME canonical production chain
// Step 8 uses (frame_id -> feature_sets | feature_artifacts pin -> FeatureArtifact
// -> keypoints[point2d_idx]) so BA and re-triangulation always optimize over
// identical measurements. Fail closed exactly like Step 8: a point whose
// observation cannot be resolved fails the stage; a source with no points
// produces an empty set (the seam evaluates nothing). The runner never invents
// observations and never falls back to source points.
std::vector<spatial::core::geometry::ReprojectionObservation>
BuildBundleAdjustmentObservations(MetadataDb& db, ArtifactStore& store,
                                  const Reconstruction& source,
                                  const json& feature_pins) {
  using spatial::core::geometry::ReprojectionObservation;
  if (source.points3D.empty()) {
    return {};
  }
  std::vector<std::pair<std::uint32_t, std::string>> frame_by_image;
  frame_by_image.reserve(source.images.size());
  for (const auto& image : source.images) {
    frame_by_image.emplace_back(image.image_id, image.frame_id);
  }

  // image_id -> resolved keypoint list (memoized), same resolution + fail-closed
  // rules as Step 8.
  std::vector<std::pair<std::uint32_t, std::vector<FeatureKeypoint>>> keypoints;
  auto resolve =
      [&](std::uint32_t image_id) -> const std::vector<FeatureKeypoint>& {
    for (const auto& entry : keypoints) {
      if (entry.first == image_id) return entry.second;
    }
    std::string frame_id;
    bool known = false;
    for (const auto& entry : frame_by_image) {
      if (entry.first == image_id) {
        frame_id = entry.second;
        known = true;
        break;
      }
    }
    if (!known || frame_id.empty()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "sparse correction: bundle-adjustment track references unknown "
          "image_id " + std::to_string(image_id));
    }
    Uuid artifact{};
    if (feature_pins.is_object() && feature_pins.contains(frame_id) &&
        feature_pins.at(frame_id).is_string()) {
      try {
        artifact = ParseUuid(feature_pins.at(frame_id).get<std::string>());
      } catch (const std::exception&) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: feature_artifacts pin for frame '" + frame_id +
                "' is not a valid artifact UUID");
      }
    } else {
      Uuid frame_uuid{};
      try {
        frame_uuid = ParseUuid(frame_id);
      } catch (const std::exception&) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: reconstruction frame_id '" + frame_id +
                "' is not a valid UUID; bundle-adjustment observations cannot "
                "be resolved");
      }
      const auto rows = db.FindFeatureSetsByFrame(frame_uuid);
      if (rows.empty()) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: frame '" + frame_id +
                "' has no feature set; bundle-adjustment observations cannot "
                "be resolved (fail closed, no fallback to source points)");
      }
      if (rows.size() > 1) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: frame '" + frame_id + "' has " +
                std::to_string(rows.size()) +
                " feature sets; pin one via feature_artifacts (ambiguous "
                "observation source fails closed)");
      }
      try {
        artifact = ParseUuid(rows.front().artifact_ref);
      } catch (const std::exception&) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: feature set of frame '" + frame_id +
                "' carries a malformed artifact_ref");
      }
    }
    const auto loaded = LoadFeatureDescriptors(store, artifact, frame_id, 0);
    if (!loaded) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "sparse correction: feature artifact of frame '" + frame_id +
              "' is absent from the CAS or is not a feature artifact");
    }
    keypoints.emplace_back(image_id, loaded->keypoints);
    return keypoints.back().second;
  };

  std::vector<ReprojectionObservation> observations;
  for (const auto& point : source.points3D) {
    for (const auto& element : point.track) {
      const std::vector<FeatureKeypoint>& kps = resolve(element.image_id);
      if (element.point2d_idx < 0 ||
          static_cast<std::size_t>(element.point2d_idx) >= kps.size()) {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: bundle-adjustment track observation point2d_idx "
            " " + std::to_string(element.point2d_idx) + " of point " +
                std::to_string(point.point3d_id) +
                " is outside the resolved feature artifact");
      }
      const auto& kp = kps[static_cast<std::size_t>(element.point2d_idx)];
      ReprojectionObservation obs;
      obs.image_id = element.image_id;
      obs.point3d_id = point.point3d_id;
      obs.keypoint_2d = Eigen::Vector2d(kp.x, kp.y);
      observations.push_back(std::move(obs));
    }
  }
  return observations;
}

// Stage 1 ("sparse_reconstruction"): images mode drives the colmap worker
// subprocess over the shared CAS; reconstruction mode forwards the canonical
// reconstruction document unchanged. Both deterministic + replayable.
void RunSparseReconstructionStage(
    ArtifactStore& store, const TaskRequest& request,
    const std::vector<std::string>& worker_command,
    const std::function<void(WorkerEvent)>& emit,
    const std::function<bool()>& cancelled) {
  const json cfg = EffectiveConfig(request);
  const std::string mode =
      cfg.value("mode", worker_command.empty() ? std::string("reconstruction")
                                               : std::string("images"));

  if (mode == "images") {
    if (worker_command.empty()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "sparse correction: images mode requires a configured worker "
          "(SPATIAL_COLMAP_WORKER + SPATIAL_COLMAP_PROBE_SHIM)");
    }
    if (request.input_refs.empty()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "sparse correction: images mode requires at least one image input");
    }

    // Composition-root wiring: this host runner is the executor boundary
    // owner for the nested worker segment (the scheduler's event loop stays
    // with the outer InProcessExecutor; the worker protocol runs here).
    ProcessExecutor worker(spatial::engine::ResourceProfile{}, worker_command,
                           "", 5000, &store);
    TaskRequest sub;
    sub.task_id = request.task_id;
    sub.task_type = request.task_type;
    sub.input_refs = request.input_refs;
    sub.expected_output_refs = request.expected_output_refs;
    sub.config_json = CleanseWorkerConfig(request.config_json).dump();
    sub.pipeline_hash = request.pipeline_hash;
    sub.workspace = request.workspace;
    worker.Submit(sub);

    ArtifactRef produced_ref;
    WorkerEvent event;
    for (;;) {
      if (!worker.WaitForEvent(event, 15000)) {
        throw ProjectError(ErrorCode::kInternal,
                           "sparse correction: worker timed out during sparse "
                           "reconstruction");
      }
      switch (event.type) {
        case WorkerEventType::kArtifactProduced:
          produced_ref = event.artifact_ref;
          break;
        case WorkerEventType::kCompleted: {
          worker.Shutdown();
          if (produced_ref.empty()) {
            throw ProjectError(
                ErrorCode::kInternal,
                "sparse correction: worker produced no reconstruction "
                "artifact");
          }
          if (cancelled()) {
            EmitCancelled(request, emit);
            return;
          }
          WorkerEvent produced;
          produced.type = WorkerEventType::kArtifactProduced;
          produced.task_id = request.task_id;
          produced.artifact_ref = produced_ref;
          emit(produced);
          EmitCompleted(request, emit);
          return;
        }
        case WorkerEventType::kFailed: {
          const std::string message = event.error_message;
          worker.Shutdown();
          throw ProjectError(
              ErrorCode::kInternal,
              "sparse correction: sparse reconstruction worker failed: " +
                  message);
        }
        case WorkerEventType::kCancelled:
          worker.Shutdown();
          EmitCancelled(request, emit);
          return;
        default:
          break;  // progress / log / heartbeat
      }
    }
  }

  // reconstruction mode: canonical reconstruction passthrough.
  if (request.input_refs.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: reconstruction mode requires one reconstruction "
        "input artifact");
  }
  const auto payload = store.Get(request.input_refs.front());
  if (!payload) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: input reconstruction artifact '" +
            request.input_refs.front() + "' is not in the artifact store");
  }
  EmitProduced(store, request, *payload, "reconstruction", request.input_refs,
               emit);
}

// Stage 2 ("sparse_correction"): the host COMMIT + stage-6 chain.
//   validate config/payload -> FindOrCreateScene -> commit v2
//     -> BundleAdjustmentOptimizePipeline (seam, D5 gate, P14) -> CAS v4
// Every precondition is checked BEFORE the first DB write (P12).
void RunSparseCorrectionStage(
    MetadataDb& db, ArtifactStore& store, const TaskRequest& request,
    const SparseCorrectionSeams& seams,
    const std::function<void(WorkerEvent)>& emit,
    const std::function<bool()>& cancelled) {
  const json cfg = EffectiveConfig(request);

  // Fail-closed validation BEFORE any database write (P12 / N3 / N5).
  const std::string project_id = cfg.value("project_id", "");
  const std::string scene_name =
      cfg.value("scene_name", std::string("p3_sparse_correction_scene"));
  const std::string random_seed = cfg.value("random_seed", "");
  if (project_id.empty()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: configuration requires a pinned "
                          "project_id (the owning project uuid)");
  }
  if (random_seed.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: configuration requires a pinned non-empty "
        "random_seed (D6)",
        /*details=*/{}, /*recoverable=*/false,
        "The same input + same seed must give the same derived metrics; the "
        "bundle-adjustment stage fails closed without it.");
  }
  if (seams.ba_optimizer == nullptr) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: no ReconstructionOptimizer seam was injected; the "
        "bundle-adjustment backend provider is unavailable");
  }
  if (request.input_refs.empty()) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: requires the sparse reconstruction input "
        "artifact");
  }
  const auto payload = store.Get(request.input_refs.front());
  if (!payload) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: input reconstruction artifact '" +
            request.input_refs.front() + "' is not in the artifact store");
  }

  Reconstruction source;
  try {
    source = ReconstructionFromJson(
        std::string(payload->begin(), payload->end()));
  } catch (const std::exception&) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: input is not a canonical reconstruction "
        "document");
  }
  if (source.status != "succeeded") {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "sparse correction: input reconstruction must carry status "
        "\"succeeded\" (got \"" +
            source.status + "\")");
  }

  // N4/N6 (runner contract, P12): inspect the scene's FULL revision history
  // BEFORE any database write. QueryLatestReconstructionByScene filters
  // succeeded rows only (metadata_db.cpp), so it cannot serve as the preflight:
  // a scene whose NEWEST reconstruction (any status) is not succeeded may not
  // be superseded by a correction. A missing scene or empty history is a
  // legitimate fresh commit.
  if (const auto scene = db.FindSceneByProject(ParseUuid(project_id));
      scene.has_value()) {
    const auto history = db.FindReconstructionsByScene(scene->scene_id);
    if (!history.empty()) {
      const auto& newest = history.back();  // ORDER BY created_at_ns ASC
      if (newest.status != "succeeded") {
        throw ValidationError(
            ErrorCode::kValidationDomain,
            "sparse correction: the scene's newest reconstruction (any status) "
            "is '" +
                newest.status +
                "'; only a succeeded newest may be superseded by the "
                "correction (N4/N6)");
      }
    }
  }
  if (cancelled()) {
    EmitCancelled(request, emit);
    return;
  }

  // P3.1 Step 4: the optional LC/trajectory segment with production-resolved
  // context (cameras from `source`, nodes/basis/candidate from the run
  // config). Runs BEFORE the first correction write (P12); strict failures
  // throw here and nothing is committed. A rejected closure persists as
  // rejected (7b) and the stage continues.
  const std::optional<VerifiedLoopClosure> verified =
      MaybeRunLoopClosureVerification(db, store, cfg, source, seams,
                                      request.pipeline_hash);
  if (cancelled()) {
    EmitCancelled(request, emit);
    return;
  }

  // Host COMMIT: materialize the reconstruction payload as a SUCCEEDED
  // revision (the only MetadataDb writer on this path; worker boundary). The
  // revision clock reuses the document's own created_at so the seam's +N
  // extension keeps the chain strictly increasing (P14 ordering).
  const SceneRow scene =
      db.FindOrCreateScene(ParseUuid(project_id), scene_name, "{}", 2000);
  const Reconstruction committed = CommitWorkerReconstructionArtifact(
      db, scene.scene_id, *payload, source.created_at_ns);
  if (cancelled()) {
    EmitCancelled(request, emit);
    return;
  }

  // P3.1 Step 5: measurement -> graph boundary. A verified metric closure
  // becomes a queryable PoseGraph (row + CAS payload) through the frozen
  // stage; visual-only/rejected closures persist nothing here (INV-3).
  std::optional<AssembledPoseGraph> assembled;
  if (verified.has_value()) {
    assembled = MaybePersistLoopClosurePoseGraph(db, store, *verified,
                                                 scene.scene_id,
                                                 request.pipeline_hash);
  }
  if (cancelled()) {
    EmitCancelled(request, emit);
    return;
  }

  // P3.1 Step 6: graph -> optimizer boundary. The persisted production graph
  // runs through the injected TrajectoryOptimizer seam; the converged result
  // is NEW persisted evidence (OptimizationResult row + CAS payload).
  std::optional<PersistedOptimization> optimized;
  if (assembled.has_value()) {
    optimized = MaybeRunTrajectoryOptimization(db, store, *assembled, seams,
                                               request.pipeline_hash);
  }
  if (cancelled()) {
    EmitCancelled(request, emit);
    return;
  }

  // P3.1 Step 7: apply boundary. The converged optimization result feeds back
  // into a NEW reconstruction revision (poses only) through the frozen seam;
  // the committed source is left succeeded for the BA P14 step below (which
  // supersedes it by id).
  std::optional<AppliedRevision> applied;
  if (optimized.has_value() && verified.has_value()) {
    applied = MaybeApplyOptimizedTrajectory(
        db, store, committed, *verified, *optimized, scene.scene_id,
        request.input_refs.front(), request.pipeline_hash);
  }
  if (cancelled()) {
    EmitCancelled(request, emit);
    return;
  }

  // P3.1 Step 8: geometry recomputation boundary. The applied revision's 3D
  // points are recomputed from the CORRECTED poses through the frozen 8b
  // re-triangulation and persisted as the v3 revision (which supersedes the
  // applied one). Geometry only — BA is a separate later step that consumes
  // this v3 (Step 9).
  const json feature_pins =
      (cfg.contains("feature_artifacts") &&
       cfg.at("feature_artifacts").is_object())
          ? cfg.at("feature_artifacts")
          : json::object();
  const std::optional<Reconstruction> retriangulated =
      applied.has_value()
          ? MaybeRetriangulateAppliedRevision(db, store, *applied,
                                              scene.scene_id, feature_pins,
                                              request.pipeline_hash)
          : std::nullopt;
  if (cancelled()) {
    EmitCancelled(request, emit);
    return;
  }

  // Step 9 — v3 -> v4 over the injected ReconstructionOptimizer seam. The BA
  // source is the retriangulated v3 when the corrected chain materialized it
  // (Step 8 output); otherwise the correction reduced to the committed
  // revision alone and BA refines IT. In the corrected chain the committed
  // revision is neither revision's source anymore, so it is chain-compacted
  // here (supersede exactly once, BEFORE the BA stage): the stage's P14 order
  // supersedes exactly its own source (v3), and a gate-failed run still leaves
  // exactly one succeeded revision (v3). Observations are resolved strictly
  // from the corrected chain's frame→feature linkage; the plain path carries
  // an empty observation list (no feature-artifact chain registered).
  if (retriangulated.has_value()) {
    MetadataDb::WriteTransaction txn(db);
    db.SetReconstructionStatus(ParseUuid(committed.reconstruction_id),
                               "superseded");
    txn.Commit();
  }
  const Reconstruction& ba_source =
      retriangulated.has_value() ? *retriangulated : committed;

  BundleAdjustmentOptimizePipelineInput in;
  in.source_v3 = &ba_source;
  in.optimizer = seams.ba_optimizer;
  in.db = &db;
  in.scene_id = scene.scene_id;
  in.random_seed = random_seed;
  // Corrected chain: observations resolved strictly from v3's frame→feature
  // linkage (the causal measurement chain). Plain path (committed-only, no
  // retriangulated): observations stay empty — no feature-artifact chain is
  // registered, so BA has nothing to refine (well-defined no-op per GA6).
  if (retriangulated.has_value()) {
    in.observations = BuildBundleAdjustmentObservations(db, store, ba_source,
                                                       feature_pins);
  }
  // else in.observations left empty (default-constructed)
  const auto out = BundleAdjustmentOptimizePipeline(in);
  if (!out.ran) {
    throw ProjectError(ErrorCode::kInternal,
                       "sparse correction: bundle adjustment did not run: " +
                           out.failure);
  }
  if (!out.gate_passed) {
    // D5: the BA source stays the ONLY succeeded revision — the gate refused
    // the refinement, so no v4 insert and no supersede of it (N1); the
    // corrected chain's committed revision is already compacted, v3 remains
    // the sole succeeded revision.
    throw ValidationError(ErrorCode::kValidationDomain,
                          "sparse correction: " + out.failure);
  }
  if (!out.v4) {
    throw ProjectError(ErrorCode::kInternal,
                       "sparse correction: bundle adjustment produced no v4");
  }
  if (cancelled()) {
    EmitCancelled(request, emit);
    return;
  }

  // The terminal output is the canonical, persisted v4 document, written back
  // to the CAS so downstream pipeline consumers can chain on it.
  const std::string document = ReconstructionToJson(*out.v4);
  EmitProduced(store, request,
               std::vector<std::uint8_t>(document.begin(), document.end()),
               "reconstruction", request.input_refs, emit);
}

}  // namespace

ResourceProfile SparseCorrectionProfile() {
  ResourceProfile profile = DemoWorkerProfile();
  profile.capabilities.push_back("sparse_reconstruction");
  profile.capabilities.push_back("bundle_adjustment");
  return profile;
}

InProcessTaskRunner MakeSparseCorrectionRunner(
    MetadataDb& db, ArtifactStore& store, SparseCorrectionSeams seams,
    std::vector<std::string> worker_command) {
  return [&db, &store, seams, worker_command = std::move(worker_command)](
             const TaskRequest& request,
             const std::function<void(WorkerEvent)>& emit,
             const std::function<bool()>& cancelled) {
    if (request.task_type == "sparse_reconstruction") {
      RunSparseReconstructionStage(store, request, worker_command, emit,
                                   cancelled);
      return;
    }
    if (request.task_type == "sparse_correction") {
      RunSparseCorrectionStage(db, store, request, seams, emit, cancelled);
      return;
    }
    throw ValidationError(ErrorCode::kInternal,
                          "sparse correction runner: unknown task_type '" +
                              request.task_type + "'");
  };
}

}  // namespace spatial::engine