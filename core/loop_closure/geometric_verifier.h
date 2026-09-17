#pragma once

// P3-impl-7b: the backend-independent GEOMETRIC LOOP-CLOSURE VERIFICATION
// contract (P3-impl-7b readiness §14).
//
// Given a LoopClosureCandidate (from 7a) plus the two resolved FeatureArtifacts
// (keypoints + descriptors), a GeometricVerifier implementation reconstructs
// image-space correspondences, estimates a robust geometric model (fundamental
// or essential), classifies inliers, assesses geometric quality, and produces a
// canonical verified LoopClosure (accepted | rejected) — or a typed verification
// failure for invalid input (P3-impl-7b §12, §13-B).
//
// Architectural invariants (P3-impl-7b §2, §13):
//  - LoopClosureCandidate != Verified LoopClosure. A descriptor similarity
//    score alone SHALL NEVER cause acceptance.
//  - Core holds the contract + canonical result semantics + acceptance policy.
//    The concrete geometric implementation (F/E + RANSAC) lives behind an
//    adapter boundary (adapters/visual_geometry) and never enters Core.
//  - No GTSAM, no PoseGraph, no trajectory/pose mutation here (7c deferred).
//  - Metric relative pose is produced ONLY when calibrated (metric) geometry
//    justifies it; uncalibrated (fundamental) verification yields NO fabricated
//    metric transform / covariance (§7, §11, §12).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/loop_closure/feature_matcher.h"
#include "core/loop_closure/verification_options.h"
#include "core/reconstruction/reconstruction.h"
#include "core/trajectory/loop_closure.h"

namespace spatial::core {

// Inputs to geometric verification: the 7a candidate, the resolved keypoints +
// descriptors of its source (newer) and target (older) frames, and the
// reconstructed image-space correspondences between them.
//
// Correspondence reconstruction (P3-impl-7b §4/§6) is backend-independent core
// logic (ReconstructCorrespondences, which uses the matcher's additive
// MatchCorrespondences seam) and is performed by the orchestrator layer BEFORE
// calling the verifier. The verifier — the geometric-estimation adapter —
// consumes the correspondences + keypoint geometry and is therefore purely
// geometric. The two feature sets must match the candidate's source/target
// frame IDs.
struct LoopClosureVerificationInput {
  LoopClosureCandidate candidate;
  MatchingFrameDescriptors source;   // newer frame (source_frame_id)
  MatchingFrameDescriptors target;   // older frame (target_frame_id)
  std::vector<FeatureCorrespondence> correspondences;  // reconstructed pairs
  // Calibrated intrinsics for the source/target frames (P3.1 Step 1/2: the
  // canonical calibration carrier is the existing ReconCamera; no parallel
  // intrinsics format is introduced). Consumed ONLY by calibrated
  // (essential) providers via CameraModel::FromReconCamera. Absent for the
  // uncalibrated (fundamental) path, which ignores these fields and keeps its
  // existing behaviour. A calibrated provider REQUIRES both cameras and
  // fails closed (rejected, no pose) when either is absent — absence is never
  // a fallback to fundamental estimation inside that provider.
  std::optional<ReconCamera> source_camera;
  std::optional<ReconCamera> target_camera;
};

// A calibrated UNIT relative pose (P3.1 Step 1/2): the cheirality-resolved
// rotation R_source_target plus the UNIT translation direction t_hat expressed
// in the source camera frame C_s. DIMENSIONLESS — this is a direction, never
// metres. Metric scale is resolved downstream ONLY by the frozen
// ResolveMetricTranslationFromUnitDirection against a metric-eligible
// trajectory; only that resolved measurement may populate the metric
// relative_position_xyz / has_relative_pose fields. Carried as
// std::optional (absent == no unit estimate) so a "present but stale" state
// is structurally impossible.
struct UnitRelativePose {
  std::array<double, 4> rotation_xyzw{0.0, 0.0, 0.0, 1.0};  // R_source_target
  std::array<double, 3> translation_direction_xyz{};        // t_hat in C_s
};

// The deterministic, canonical result of one geometric verification run
// (P3-impl-7b §7, §9). Contains everything needed to distinguish REJECTED from
// ACCEPTED/VERIFIED and to record the geometric evidence. Identity fields
// (closure_id, created_at_ns) and provenance on `closure` are owned by the
// caller/orchestration layer (instance identity, D-DI-01), mirroring 7a.
struct GeometricVerificationResult {
  // Accepted/rejected outcome gate.
  bool verified = false;

  // Canonical output record. status = "accepted" | "rejected"; deterministic
  // fields (inlier_ratio, inlier_count, confidence, temporal/spatial
  // separation) are filled by the verifier. This type carries NO transform /
  // covariance fields (P3-impl-7b §11): metric pose + uncertainty appear only
  // on a 7c PoseGraphEdge, and only when metric evidence justified them.
  LoopClosure closure;

  // Which geometric model was used: "fundamental" (uncalibrated consistency),
  // "essential" (calibrated -> metric-eligible), or "none" (no model). Explicit
  // so the caller never assumes a model the evidence did not supply (§8).
  std::string geometric_model;

  // Quality/evidence metrics (§9).
  std::size_t correspondence_count = 0;  // reconstructed correspondences
  std::size_t inlier_count = 0;          // geometrically consistent inliers
  double inlier_ratio = 0.0;             // inlier_count / correspondence_count
  double geometric_residual = 0.0;       // mean Sampson residual (pixels)
  double confidence = 0.0;               // combined confidence [0,1]

  // Populated whenever !verified to explain the outcome (§12 failure is
  // explicit; never a silent false-positive accept).
  std::string rejection_reason;

  // Relative pose recovered from the estimated geometry. Present ONLY when
  // metric (calibrated/essential) evidence justified it (§7, §11). For the
  // uncalibrated (fundamental) first path this is always false — a genuinely
  // non-metric result is never dressed up as a metric pose.
  //
  // UNIT-pose rule (P3.1 Step 1/2): a calibrated essential provider that
  // estimates R + t_hat (unit direction) sets has_relative_pose = false and
  // leaves relative_position_xyz zeroed — a unit direction MUST NEVER be
  // placed in these metric fields (downstream LoopClosureToPoseGraph reads
  // them as metres). The unit estimate travels ONLY in unit_relative_pose
  // below until the frozen metric resolver produces metres.
  bool has_relative_pose = false;
  std::array<double, 3> relative_position_xyz{};   // T_source_target translation
  std::array<double, 4> relative_rotation_xyzw{};  // T_source_target (x,y,z,w)

  // Calibrated unit relative pose (essential path only). Set on a successful
  // calibrated estimation; stays nullopt on every rejection/failure and for
  // the uncalibrated path. geometric_model == "essential" marks the producer.
  std::optional<UnitRelativePose> unit_relative_pose;
};

// Backend-independent verification seam (P3-impl-7b §14). Core never names a
// concrete geometric algorithm. The engine selects an implementation by
// capability (ADR-011/034). Implementations MUST be deterministic for identical
// inputs + options (D-DI-02).
class GeometricVerifier {
 public:
  virtual ~GeometricVerifier() = default;

  // Verifies a loop-closure candidate against the resolved feature evidence.
  // Returns a canonical GeometricVerificationResult. Throws
  // ProjectError(kValidationDomain) for invalid input (malformed feature data,
  // index out of range, empty descriptors, dimension mismatch, ...) per
  // P3-impl-7b §13-B — as opposed to a geometric REJECTION (valid input whose
  // geometry does not support the loop).
  virtual GeometricVerificationResult Verify(
      const LoopClosureVerificationInput& input,
      const GeometricVerificationOptions& options) const = 0;
};

}  // namespace spatial::core
