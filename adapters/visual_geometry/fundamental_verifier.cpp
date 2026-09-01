#include "adapters/visual_geometry/fundamental_verifier.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "core/errors/project_error.h"

namespace spatial::adapters::visual_geometry {

namespace {

using spatial::core::ErrorCode;
using spatial::core::FeatureCorrespondence;
using spatial::core::GeometricVerificationResult;
using spatial::core::GeometricVerificationOptions;
using spatial::core::LoopClosure;
using spatial::core::LoopClosureCandidate;
using spatial::core::LoopClosureVerificationInput;
using spatial::core::MatchingFrameDescriptors;

// --- Hartley normalization (deterministic) ---------------------------------
// Translates the points to their centroid and scales so the mean distance to
// the origin is sqrt(2) — the standard well-conditioning for the 8-point
// algorithm. Same T transforms the set's homogeneous points: p' = T p.
struct Norm {
  Eigen::Matrix3d T;
  // Returns true when the set is degenerate (e.g. all points coincident or
  // a zero spread), in which case no meaningful normalization is possible.
  bool degeneracy_guard_needed = false;
};

Norm Normalize(const std::vector<Eigen::Vector3d>& pts) {
  Norm n;
  n.T = Eigen::Matrix3d::Identity();
  if (pts.size() < 2) return n;

  Eigen::Vector3d centroid_sum = Eigen::Vector3d::Zero();
  for (const auto& p : pts) centroid_sum += p;
  const Eigen::Vector3d mean =
      centroid_sum / static_cast<double>(pts.size());
  double mean_dist = 0.0;
  for (const auto& p : pts)
    mean_dist += std::hypot(p.x() - mean.x(), p.y() - mean.y());
  mean_dist /= static_cast<double>(pts.size());
  if (!std::isfinite(mean_dist) || mean_dist < 1e-12) {
    n.degeneracy_guard_needed = true;
    return n;
  }
  const double s = std::sqrt(2.0) / mean_dist;
  n.T << s, 0.0, -s * mean.x(),  //
      0.0, s, -s * mean.y(),     //
      0.0, 0.0, 1.0;
  return n;
}

// 8-point fundamental-matrix estimate from >= 8 correspondences (after
// normalization + denormalization). Returns true on success.
bool EstimateFundamental(
    const std::vector<Eigen::Vector3d>& src,
    const std::vector<Eigen::Vector3d>& tgt,
    Eigen::Matrix3d& F) {
  const Norm ns = Normalize(src);
  const Norm nt = Normalize(tgt);
  if (ns.degeneracy_guard_needed || nt.degeneracy_guard_needed) return false;

  const std::size_t n = src.size();
  Eigen::MatrixXd A(n, 9);
  for (std::size_t i = 0; i < n; ++i) {
    const Eigen::Vector3d x = ns.T * src[i];
    const Eigen::Vector3d xp = nt.T * tgt[i];
    A.row(static_cast<Eigen::Index>(i)) << xp.x() * x.x(), xp.x() * x.y(),
        xp.x(), xp.y() * x.x(), xp.y() * x.y(), xp.y(), x.x(), x.y(), 1.0;
  }

  const Eigen::JacobiSVD<Eigen::MatrixXd> svd(
      A, Eigen::ComputeFullV);
  const Eigen::VectorXd v = svd.matrixV().col(8);
  Eigen::Matrix3d Fn;
  Fn << v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8];

  // Enforce rank 2 (epipolar constraint).
  const Eigen::JacobiSVD<Eigen::Matrix3d> svd2(
      Fn, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Eigen::Vector3d s = svd2.singularValues();
  s(2) = 0.0;
  Fn = svd2.matrixU() * s.asDiagonal() * svd2.matrixV().transpose();

  // Denormalize: F = T_tgt^T * Fn * T_src.
  F = nt.T.transpose() * Fn * ns.T;

  if (!F.allFinite()) return false;
  // Scale so F(2,2) == 1 (removes a homogeneous ambiguity, aids determinism).
  if (std::abs(F(2, 2)) < 1e-12) return false;
  F /= F(2, 2);
  return true;
}

// Sampson distance (squared) of a correspondence under F.
double SampsonSq(const Eigen::Vector3d& x, const Eigen::Vector3d& xp,
                 const Eigen::Matrix3d& F) {
  const Eigen::Vector3d Fx = F * x;
  const Eigen::Vector3d Ftxp = F.transpose() * xp;
  const double denom = Fx[0] * Fx[0] + Fx[1] * Fx[1] +
                       Ftxp[0] * Ftxp[0] + Ftxp[1] * Ftxp[1];
  if (denom < 1e-12) return std::numeric_limits<double>::infinity();
  const double e = xp.dot(Fx);
  return (e * e) / denom;
}

// A robust, deterministic best-model search seeded by the requested iteration
// count (kept bounded). Returns the inlier mask for the best fundamental model
// found, or an empty mask when no valid model (>= 8 correspondences) exists.
struct RansacOutcome {
  bool ok = false;               // a valid F was estimated and refined
  Eigen::Matrix3d F;
  std::vector<bool> inlier;      // valid when ok
  std::size_t inlier_count = 0;
  double mean_residual_sq = 0.0; // mean inlier Sampson distance (px)
};

RansacOutcome RobustFundamental(
    const std::vector<Eigen::Vector3d>& src,
    const std::vector<Eigen::Vector3d>& tgt,
    const GeometricVerificationOptions& opts) {
  RansacOutcome out;
  const std::size_t n = src.size();
  if (n < 8) return out;  // not enough for an 8-point model

  // Fixed seed (not wall-clock) => identical inputs yield identical RANSAC.
  // The 32-bit seed is truncated from a documented constant.
  std::mt19937 rng(0x5EED7B12u);
  // Reference set = all correspondences (we sample from them below).
  std::vector<std::size_t> idx(n);
  for (std::size_t i = 0; i < n; ++i) idx[i] = i;

  Eigen::Matrix3d best_F;
  std::vector<bool> best_inlier;
  std::size_t best_count = 0;
  double best_residual_sq = std::numeric_limits<double>::infinity();

  std::size_t iterations = std::max<std::size_t>(1, opts.max_ransac_iterations);
  const std::size_t sample_size = 8;
  const double thresh_sq =
      opts.ransac_inlier_threshold * opts.ransac_inlier_threshold;

  for (std::size_t iter = 0; iter < iterations; ++iter) {
    // Deterministic sample of `sample_size` distinct indices.
    std::vector<std::size_t> chosen;
    chosen.reserve(sample_size);
    std::vector<bool> used(n, false);
    std::size_t guard = 0;
    while (chosen.size() < sample_size && guard < n * 4u) {
      const std::size_t r = static_cast<std::size_t>(
          rng() % static_cast<unsigned long>(n));
      if (!used[r]) {
        used[r] = true;
        chosen.push_back(r);
      }
      ++guard;
    }
    if (chosen.size() < sample_size) continue;

    std::vector<Eigen::Vector3d> s_samp, t_samp;
    s_samp.reserve(sample_size);
    t_samp.reserve(sample_size);
    for (const std::size_t r : chosen) {
      s_samp.push_back(src[r]);
      t_samp.push_back(tgt[r]);
    }

    Eigen::Matrix3d F;
    if (!EstimateFundamental(s_samp, t_samp, F)) continue;

    // Score on the full set.
    std::vector<bool> inl(n, false);
    std::size_t cnt = 0;
    double resid_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double d = SampsonSq(src[i], tgt[i], F);
      if (d <= thresh_sq && std::isfinite(d)) {
        inl[i] = true;
        ++cnt;
        resid_sum += std::sqrt(d);
      }
    }
    if (cnt > best_count ||
        (cnt == best_count && cnt > 0 && best_count > 0 &&
         (resid_sum / static_cast<double>(cnt)) < best_residual_sq)) {
      best_count = cnt;
      best_inlier = std::move(inl);
      best_F = F;
      best_residual_sq =
          (cnt > 0) ? resid_sum / static_cast<double>(cnt) : 0.0;
    }
  }

  if (best_count < 8 || best_inlier.empty()) return out;

  // Refine: re-estimate F on the inlier set (deterministic, single re-fit).
  std::vector<Eigen::Vector3d> s_in, t_in;
  s_in.reserve(best_count);
  t_in.reserve(best_count);
  for (std::size_t i = 0; i < n; ++i)
    if (best_inlier[i]) {
      s_in.push_back(src[i]);
      t_in.push_back(tgt[i]);
    }
  Eigen::Matrix3d refined;
  if (!EstimateFundamental(s_in, t_in, refined)) return out;

  // Re-score inliers under the refined model.
  std::vector<bool> inl(n, false);
  std::size_t cnt = 0;
  double resid_sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double d = SampsonSq(src[i], tgt[i], refined);
    if (d <= thresh_sq && std::isfinite(d)) {
      inl[i] = true;
      ++cnt;
      resid_sum += std::sqrt(d);
    }
  }
  if (cnt < 8) return out;

  out.ok = true;
  out.F = refined;
  out.inlier = std::move(inl);
  out.inlier_count = cnt;
  out.mean_residual_sq = resid_sum / static_cast<double>(cnt);
  return out;
}

}  // namespace

GeometricVerificationResult FundamentalGeometricVerifier::Verify(
    const LoopClosureVerificationInput& input,
    const GeometricVerificationOptions& options) const {
  GeometricVerificationResult res;
  const LoopClosureCandidate& cand = input.candidate;
  res.closure.trajectory_id = cand.trajectory_id;
  res.closure.candidate_id = cand.candidate_id;
  res.closure.source_frame_id = cand.source_frame_id;
  res.closure.target_frame_id = cand.target_frame_id;
  res.closure.status = "rejected";
  res.rejection_reason = "temporal_separation";

  const MatchingFrameDescriptors& src = input.source;
  const MatchingFrameDescriptors& tgt = input.target;

  // --- Input validation (P3-impl-7b §13-B): malformed -> typed error. ---
  if (src.descriptors.empty() || tgt.descriptors.empty() ||
      src.keypoints.size() != src.descriptors.size() ||
      tgt.keypoints.size() != tgt.descriptors.size()) {
    throw spatial::core::ProjectError(
        ErrorCode::kValidationDomain,
        "loop closure verifier: malformed feature data (empty or keypoint/"
        "descriptor count mismatch)");
  }
  if (src.frame_id != cand.source_frame_id ||
      tgt.frame_id != cand.target_frame_id) {
    throw spatial::core::ProjectError(
        ErrorCode::kValidationDomain,
        "loop closure verifier: feature frame id does not match candidate");
  }

  const std::vector<FeatureCorrespondence>& corr = input.correspondences;
  for (const auto& c : corr) {
    if (c.source_index >= src.descriptors.size() ||
        c.target_index >= tgt.descriptors.size()) {
      throw spatial::core::ProjectError(
          ErrorCode::kValidationDomain,
          "loop closure verifier: out-of-range correspondence index");
    }
  }

  res.correspondence_count = corr.size();

  // --- D-LC-06 temporal separation gate. ---
  const std::int64_t sep = src.timestamp_ns - tgt.timestamp_ns;
  res.closure.temporal_separation_ns = sep < 0 ? -sep : sep;
  if (res.closure.temporal_separation_ns <
      options.minimum_temporal_separation_ns) {
    res.closure.status = "rejected";
    res.rejection_reason = "temporal_separation_below_minimum";
    return res;
  }

  // --- Insufficient correspondences -> reject (fail closed). ---
  if (corr.size() < options.min_correspondences) {
    res.closure.status = "rejected";
    res.rejection_reason = "insufficient_correspondences";
    return res;
  }

  // --- Robust geometric estimation (F + RANSAC). ---
  std::vector<Eigen::Vector3d> src_pts, tgt_pts;
  src_pts.reserve(corr.size());
  tgt_pts.reserve(corr.size());
  for (const auto& c : corr) {
    src_pts.emplace_back(src.keypoints[c.source_index].x,
                         src.keypoints[c.source_index].y, 1.0);
    tgt_pts.emplace_back(tgt.keypoints[c.target_index].x,
                         tgt.keypoints[c.target_index].y, 1.0);
  }

  const RansacOutcome ransac = RobustFundamental(src_pts, tgt_pts, options);
  res.geometric_model = "fundamental";
  if (!ransac.ok) {
    res.closure.status = "rejected";
    res.rejection_reason = "no_valid_model";
    res.inlier_count = 0;
    res.inlier_ratio = 0.0;
    return res;
  }

  res.inlier_count = ransac.inlier_count;
  res.inlier_ratio = static_cast<double>(ransac.inlier_count) /
                     static_cast<double>(corr.size());
  res.geometric_residual = ransac.mean_residual_sq;

  // --- Quality gates (P3-impl-7b §10). ---
  const bool enough_inliers = ransac.inlier_count >= options.min_inliers;
  const bool enough_ratio =
      res.inlier_ratio >= options.min_inlier_ratio - 1e-9;
  const bool residual_ok =
      res.geometric_residual <= options.max_residual;

  // Combined confidence (deterministic): dominated by inlier ratio, then
  // damped by residual quality (a high residual reduces confidence). Kept in
  // [0,1]. This is an acceptance/reliability measure, NOT covariance (§12).
  double conf = res.inlier_ratio;
  if (options.max_residual > 0.0) {
    const double quality =
        std::clamp(1.0 - res.geometric_residual / options.max_residual, 0.0,
                   1.0);
    conf *= 0.5 + 0.5 * quality;
  }
  res.confidence = std::clamp(conf, 0.0, 1.0);
  res.closure.confidence = res.confidence;
  res.closure.inlier_count = ransac.inlier_count;
  res.closure.inlier_ratio = res.inlier_ratio;

  if (!enough_inliers) {
    res.closure.status = "rejected";
    res.rejection_reason = "below_minimum_inliers";
    return res;
  }
  if (!enough_ratio) {
    res.closure.status = "rejected";
    res.rejection_reason = "below_minimum_inlier_ratio";
    return res;
  }
  if (!residual_ok) {
    res.closure.status = "rejected";
    res.rejection_reason = "geometric_error_above_max";
    return res;
  }
  if (res.confidence < options.verify_min_confidence) {
    res.closure.status = "rejected";
    res.rejection_reason = "below_minimum_confidence";
    return res;
  }

  // Accepted. Uncalibrated (fundamental) verification carries NO metric pose.
  res.verified = true;
  res.closure.status = "accepted";
  res.has_relative_pose = false;  // fundamental only: no metric scale (§7, §11)
  res.rejection_reason.clear();
  return res;
}

}  // namespace spatial::adapters::visual_geometry
