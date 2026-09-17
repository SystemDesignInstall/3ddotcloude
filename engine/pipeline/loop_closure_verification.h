#pragma once

// P3-impl-7b: the Geometric Loop-Closure Verification pipeline stage.
//
// Contract:
//   LoopClosureCandidate        (from 7a)
//        -> resolve source + target FeatureArtifacts (keypoints + descriptors)
//        -> ReconstructCorrespondences (matcher seam)
//        -> GeometricVerifier (adapter: fundamental + RANSAC)
//        -> canonical LoopClosure (accepted | rejected)
//        -> metadata row (existing loop_closures) + CAS payload
//           (loop-closure.schema.json, closures populated) + provenance manifest
//
// This mirrors the relationship established in 7a: core/loop_closure holds the
// canonical, scene-agnostic verification contract; adapters/visual_geometry
// holds the concrete geometric implementation; this engine layer resolves the
// candidate's FeatureArtifacts, injects the matcher + verifier, stamps
// instance identity + provenance, and persists the result.
//
// No PoseGraph assembly or GTSAM here (that is P3-impl-7c, deferred).
// Unit-to-metric promotion (P3.1 Step 3) IS here: after a calibrated verifier
// yields a UNIT pose, this stage resolves it to metres via the FROZEN
// ResolveMetricTranslationFromUnitDirection — but only onto a
// metric-eligible trajectory (INV-3). No optimizer, no graph assembly.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/artifacts/artifact_store.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/loop_closure/geometric_verifier.h"
#include "core/reconstruction/reconstruction.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/trajectory/metric_basis.h"
#include "core/trajectory/trajectory.h"

namespace spatial::engine {

// A frame's identity, timestamp, and the 16-byte UUID of its FeatureArtifact
// in the CAS — the inputs needed to resolve the geometry for verification.
// `camera` carries the frame's calibrated intrinsics (canonical ReconCamera)
// for calibrated (essential) providers; nullopt means uncalibrated (the
// fundamental path ignores it; a calibrated provider fails closed without it).
struct VerificationFrameInput {
  spatial::core::Uuid frame_id{};
  std::int64_t timestamp_ns = 0;
  spatial::core::Uuid feature_artifact_uuid{};
  std::optional<spatial::core::ReconCamera> camera;
};

// Trajectory context for unit-to-metric resolution (P3.1 Step 3). Borrowed,
// never owned; the caller (later: the correction pipeline) supplies the
// trajectory's own nodes + its provenance-derived metric_basis declaration.
// Nullopt (the default) means "no trajectory context": verification runs and
// a unit pose may be estimated, but no metric promotion is attempted.
struct MetricResolutionInput {
  const std::vector<spatial::core::TrajectoryPoseNode>* trajectory_nodes =
      nullptr;
  spatial::core::MetricBasis metric_basis;
};

// Result of one geometric verification run: the canonical verified/rejected
// LoopClosure, the deterministic geometric diagnostics, and the CAS artifact
// (loop-closure payload) + metadata evidence.
struct LoopClosureVerificationResult {
  spatial::core::LoopClosure closure;      // canonical output
  spatial::core::GeometricVerificationResult geo;  // geometric evidence/diagnostics
  std::string payload_content_hash;        // CAS content hash (loop-closure payload)
  spatial::core::Uuid payload_artifact_uuid{};
  bool artifacts_resolved = false;         // both FeatureArtifacts were resolved
};

// Verifies one loop-closure candidate geometrically: resolves the candidate's
// source + target FeatureArtifacts, reconstructs image-space correspondences,
// runs the injected backend-independent GeometricVerifier, stamps instance
// identity (closure_id / created_at_ns) and provenance, persists the canonical
// LoopClosure to the existing loop_closures metadata table, and writes a
// loop-closure CAS payload (candidates + closures) with an artifact manifest
// whose input_artifact_hashes reference the source FeatureArtifacts.
//
// Failure semantics (P3-impl-7b §12, §13): a geometrically REJECTED closure is
// a normal result (returned, status="rejected"). Missing/malformed feature
// data is a verification FAILURE and throws ProjectError(kValidationDomain).
//
// Unit-to-metric promotion (P3.1 Step 3): when `metric_resolution` is present,
// the trajectory is metric-eligible (INV-3), the closure is accepted, and the
// verifier produced a UNIT pose, the FROZEN D6 resolver converts the unit
// direction to metres and the closure is promoted (has_relative_pose=true,
// metric relative_position_xyz/rotation). The verifier's own `geo` record is
// left exactly as produced (geo.has_relative_pose stays false on the unit
// path): estimation evidence and orchestrated resolution stay distinguishable.
// Basis-absent or resolver-rejected stays verified-visual-only (persistable,
// no metric edge downstream).
LoopClosureVerificationResult VerifyLoopClosureGeometry(
    spatial::core::ArtifactStore& store, spatial::core::MetadataDb& db,
    const spatial::core::LoopClosureCandidate& candidate,
    const VerificationFrameInput& source, const VerificationFrameInput& target,
    const spatial::core::LoopClosureFeatureMatcher& matcher,
    const spatial::core::GeometricVerifier& verifier,
    const spatial::core::GeometricVerificationOptions& options,
    const std::string& configuration_hash = "",
    const std::optional<MetricResolutionInput>& metric_resolution =
        std::nullopt);

}  // namespace spatial::engine
