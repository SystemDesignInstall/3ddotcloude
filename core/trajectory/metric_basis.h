#pragma once

// Metric-basis declaration (P3 Phase 3, D4).
//
// Declares that a trajectory's poses live in a trusted metric scale. The
// declaration is PROVENANCE-DERIVED, never caller-asserted (invariant
// [max-conviction], P3-impl-7c §5.1): only the scale-producing pipeline
// (odometry adapter, calibrated-baseline stage, metric COLMAP alignment) can
// set it, and its provenance record is the evidence.
//
// A valid `declared == true` basis is the eligibility gate for a metric
// loop-closure edge (INV-3). Absent / undeclared basis:
//   verified visual closure  ->  persistable  ->  NO PoseGraph metric edge
//
// Validation is deterministic and lives in core (`ValidateMetricBasis`).

#include <string>

#include "core/reconstruction/reconstruction.h"

namespace spatial::core {

// How the metric scale basis was established. Enum backing the schema's
// `basis` string field (P3-impl-7c §5.1).
enum class MetricBasisType : uint8_t {
  kOdometryScale = 0,        // "odometry_scale"
  kCalibratedBaseline = 1,   // "calibrated_baseline"
  kSfmMetricAligned = 2,     // "sfm_metric_aligned"
  kGroundTruth = 3,          // "ground_truth"
};

// Which artifact declared the scale basis (schema `source` string field).
enum class MetricBasisSource : uint8_t {
  kTrajectory = 0,       // "trajectory"
  kReconstruction = 1,   // "reconstruction"
  kCombined = 2,         // "combined"
};

inline const char* MetricBasisTypeName(MetricBasisType t) noexcept {
  switch (t) {
    case MetricBasisType::kOdometryScale:
      return "odometry_scale";
    case MetricBasisType::kCalibratedBaseline:
      return "calibrated_baseline";
    case MetricBasisType::kSfmMetricAligned:
      return "sfm_metric_aligned";
    case MetricBasisType::kGroundTruth:
      return "ground_truth";
    default:
      return "unknown";
  }
}

inline const char* MetricBasisSourceName(MetricBasisSource s) noexcept {
  switch (s) {
    case MetricBasisSource::kTrajectory:
      return "trajectory";
    case MetricBasisSource::kReconstruction:
      return "reconstruction";
    case MetricBasisSource::kCombined:
      return "combined";
    default:
      return "unknown";
  }
}

// Metric-basis declaration stored on the Trajectory root (D4). Mirrors the
// `metric_basis` block of trajectory.schema.json.
struct MetricBasis {
  bool declared = false;                  // true only via a trusted scale pipeline
  MetricBasisSource source = MetricBasisSource::kTrajectory;
  MetricBasisType basis = MetricBasisType::kOdometryScale;
  ReconstructionProvenance provenance;    // D-CRM-11 provenance (the evidence)
  std::string scale_calibration_ref;      // CAS/artifact ref that fixes scale
  bool operator==(const MetricBasis&) const = default;
};

// Deterministic validation of a metric-basis declaration (D4, §5.1).
// `ok == true` for an undeclared basis (a well-formed non-metric state) and
// for a fully-specified declared basis. A bare `declared: true` with no
// scale_calibration_ref or no provenance.configuration_hash is REJECTED
// (<strike>fabricated</strike> by-fiat basis) -> `ok == false`.
struct MetricBasisCheck {
  bool ok = false;
  std::string reason;                     // human-readable, stable
};

inline MetricBasisCheck ValidateMetricBasis(const MetricBasis& mb) {
  if (!mb.declared) return {true, "undeclared"};
  if (mb.scale_calibration_ref.empty()) {
    return {false, "declared metric basis missing scale_calibration_ref"};
  }
  if (mb.provenance.configuration_hash.empty()) {
    return {false, "declared metric basis missing provenance.configuration_hash"};
  }
  return {true, "metric"};
}

// The metric-eligibility gate used by BuildMetricLoopClosureEdge (INV-3):
// a trajectory is metric-eligible only when its basis is DECLARED and the
// declaration validates. Undeclared or by-fiat bases are not eligible.
inline bool MetricEligibleTrajectoryBasis(const MetricBasis& mb) {
  return mb.declared && ValidateMetricBasis(mb).ok;
}

}  // namespace spatial::core
