#include "adapters/visual_geometry/essential_verifier.h"

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
#include "core/geometry/camera_model.h"
#include "core/geometry/quaternion.h"

namespace spatial::adapters::visual_geometry {

namespace {

using spatial::core::ErrorCode;
using spatial::core::FeatureCorrespondence;
using spatial::core::FeatureKeypoint;
using spatial::core::GeometricVerificationOptions;
using spatial::core::GeometricVerificationResult;
using spatial::core::LoopClosure;
using spatial::core::LoopClosureCandidate;
using spatial::core::LoopClosureVerificationInput;
using spatial::core::MatchingFrameDescriptors;
using spatial::core::ReconCamera;
using spatial::core::geometry::CameraModel;
using spatial::core::geometry::PixelToNormalized;

// --- Intrinsics matrix ------------------------------------------------------
// K from canonical ReconCamera fields (same vocabulary consumed by
// CameraModel::FromReconCamera; no parallel format).
Eigen::Matrix3d IntrinsicsMatrix(const ReconCamera& camera) {
  Eigen::Matrix3d K = Eigen::Matrix3d::Zero();
  K(0, 0) = camera.fx;
  K(1, 1) = camera.fy;
  K(0, 2) = camera.cx;
  K(1, 2) = camera.cy;
  K(2, 2) = 1.0;
  return K;
}

// --- Calibrated normalization (Step 2B) -------------------------------------
// Observed (distorted) pixel -> undistorted NORMALIZED coordinate via the
// validated CameraModel. Throws the model's typed errors for degenerate use.
Eigen::Vector3d NormalizedPoint(const CameraModel& model,
                                const FeatureKeypoint& keypoint) {
  const Eigen::Vector2d pixel(keypoint.x, keypoint.y);
  const Eigen::Vector2d xn =
      model.UndistortUnit(PixelToNormalized(model.intrinsics(), pixel));
  return Eigen::Vector3d(xn.x(), xn.y(), 1.0);
}

// --- Hartley normalization (deterministic, mirrors fundamental_verifier) ----
struct Norm {
  Eigen::Matrix3d T;
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

// 8-point essential-matrix estimate from >= 8 NORMALIZED correspondences
// (after normalization + denormalization). Returns true on success.
bool EstimateEssential(const std::vector<Eigen::Vector3d>& src,
                       const std::vector<Eigen::Vector3d>& tgt,
                       Eigen::Matrix3d& E) {
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

  const Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
  const Eigen::VectorXd v = svd.matrixV().col(8);
  Eigen::Matrix3d En;
  En << v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8];

  // Denormalize: E = T_tgt^T * En * T_src.
  E = nt.T.transpose() * En * ns.T;

  if (!E.allFinite()) return false;
  if (E.norm() < 1e-12) return false;  // vanishing matrix: no constraint
  return true;
}

// Sampson distance (squared) of a correspondence under F (pixel space).
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

// A robust, deterministic best-model search (mirrors RobustFundamental).
// Estimates E on normalized coordinates but scores in PIXEL space via
// F = K_t^-T * E * K_s^-1, so the shared pixel-space options and residual
// semantics apply unchanged.
struct RansacOutcome {
  bool ok = false;  // a valid E was estimated and refined
  Eigen::Matrix3d E;
  std::vector<bool> inlier;  // valid when ok
  std::size_t inlier_count = 0;
  double mean_residual_px = 0.0;  // mean inlier Sampson distance (px)
};

RansacOutcome RobustEssential(const std::vector<Eigen::Vector3d>& src_norm,
                              const std::vector<Eigen::Vector3d>& tgt_norm,
                              const std::vector<Eigen::Vector3d>& src_px,
                              const std::vector<Eigen::Vector3d>& tgt_px,
                              const Eigen::Matrix3d& Ks_inv,
                              const Eigen::Matrix3d& Kt_inv_T,
                              const GeometricVerificationOptions& opts) {
  RansacOutcome out;
  const std::size_t n = src_norm.size();
  if (n < 8) return out;  // not enough for an 8-point model

  // Fixed seed (not wall-clock) => identical inputs yield identical RANSAC.
  // Distinct constant from the fundamental verifier's seed (D-DI-02).
  std::mt19937 rng(0xE55E971Au);
  std::vector<std::size_t> idx(n);
  for (std::size_t i = 0; i < n; ++i) idx[i] = i;

  Eigen::Matrix3d best_E;
  std::vector<bool> best_inlier;
  std::size_t best_count = 0;
  double best_residual_px = std::numeric_limits<double>::infinity();

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
      s_samp.push_back(src_norm[r]);
      t_samp.push_back(tgt_norm[r]);
    }

    Eigen::Matrix3d E;
    if (!EstimateEssential(s_samp, t_samp, E)) continue;

    // Score on the full set in pixel space.
    const Eigen::Matrix3d F = Kt_inv_T * E * Ks_inv;
    std::vector<bool> inl(n, false);
    std::size_t cnt = 0;
    double resid_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double d = SampsonSq(src_px[i], tgt_px[i], F);
      if (d <= thresh_sq && std::isfinite(d)) {
        inl[i] = true;
        ++cnt;
        resid_sum += std::sqrt(d);
      }
    }
    if (cnt > best_count ||
        (cnt == best_count && cnt > 0 && best_count > 0 &&
         (resid_sum / static_cast<double>(cnt)) < best_residual_px)) {
      best_count = cnt;
      best_inlier = std::move(inl);
      best_E = E;
      best_residual_px =
          (cnt > 0) ? resid_sum / static_cast<double>(cnt) : 0.0;
    }
  }

  if (best_count < 8 || best_inlier.empty()) return out;

  // Refine: re-estimate E on the inlier set (deterministic, single re-fit).
  std::vector<Eigen::Vector3d> s_in, t_in;
  s_in.reserve(best_count);
  t_in.reserve(best_count);
  for (std::size_t i = 0; i < n; ++i)
    if (best_inlier[i]) {
      s_in.push_back(src_norm[i]);
      t_in.push_back(tgt_norm[i]);
    }
  Eigen::Matrix3d refined;
  if (!EstimateEssential(s_in, t_in, refined)) return out;

  // Re-score inliers under the refined model.
  const Eigen::Matrix3d F = Kt_inv_T * refined * Ks_inv;
  std::vector<bool> inl(n, false);
  std::size_t cnt = 0;
  double resid_sum = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double d = SampsonSq(src_px[i], tgt_px[i], F);
    if (d <= thresh_sq && std::isfinite(d)) {
      inl[i] = true;
      ++cnt;
      resid_sum += std::sqrt(d);
    }
  }
  if (cnt < 8) return out;

  out.ok = true;
  out.E = refined;
  out.inlier = std::move(inl);
  out.inlier_count = cnt;
  out.mean_residual_px = resid_sum / static_cast<double>(cnt);
  return out;
}

// --- Essential decomposition (Step 2D, Hartley-Zisserman) -------------------
// E = U diag(1,1,0) V^T; R1 = U W V^T, R2 = U W^T V^T, t = u3 (unit).
// Determinant fixes keep proper rotations (E is defined up to sign; the
// cheirality vote below disambiguates the remaining 4-fold ambiguity).
struct EssentialDecomposition {
  Eigen::Matrix3d R1;
  Eigen::Matrix3d R2;
  Eigen::Vector3d t;  // unit
};

EssentialDecomposition DecomposeEssential(const Eigen::Matrix3d& E_raw) {
  const Eigen::JacobiSVD<Eigen::Matrix3d> svd(
      E_raw, Eigen::ComputeFullU | Eigen::ComputeFullV);
  const Eigen::Matrix3d U = svd.matrixU();
  const Eigen::Matrix3d V = svd.matrixV();

  Eigen::Matrix3d W = Eigen::Matrix3d::Zero();
  W(0, 1) = -1.0;
  W(1, 0) = 1.0;
  W(2, 2) = 1.0;

  EssentialDecomposition d;
  d.R1 = U * W * V.transpose();
  d.R2 = U * W.transpose() * V.transpose();
  if (d.R1.determinant() < 0.0) d.R1 = -d.R1;
  if (d.R2.determinant() < 0.0) d.R2 = -d.R2;
  d.t = U.col(2).normalized();
  return d;
}

// --- Cheirality vote (Step 2E) ----------------------------------------------
// Linear least-squares triangulation of a unit-ray pair under candidate
// (R, t): X_s = a r_s, X_t = b r_t, X_t = R X_s + t. Returns the depths
// (z_s, z_t); ok=false for parallel rays or non-finite solves.
struct Depths {
  bool ok = false;
  double z_s = 0.0;
  double z_t = 0.0;
};

Depths TriangulateUnitRays(const Eigen::Vector3d& r_s,
                           const Eigen::Vector3d& r_t,
                           const Eigen::Matrix3d& R,
                           const Eigen::Vector3d& t) {
  Depths out;
  const Eigen::Vector3d u = r_s;
  const Eigen::Vector3d v = R.transpose() * r_t;
  const Eigen::Vector3d w = -R.transpose() * t;
  // [u, -v] [a, b]^T = w  ->  2x2 normal equations.
  const double A = u.dot(u);
  const double B = -u.dot(v);
  const double C = v.dot(v);
  const double det = A * C - B * B;
  if (!std::isfinite(det) || std::abs(det) < 1e-12) return out;
  const double uw = u.dot(w);
  const double vw = v.dot(w);
  // Inverse of [[A,B],[B,C]] applied to [uw, -vw]: a = (C*uw + B*vw)/det,
  // b = -(B*uw + A*vw)/det. (A previous revision had the second terms
  // sign-flipped, which negated all depths and inverted the vote; T3/T4 pin
  // the correct signs.)
  const double a = (C * uw + B * vw) / det;
  const double b = -(B * uw + A * vw) / det;
  if (!std::isfinite(a) || !std::isfinite(b)) return out;
  const Eigen::Vector3d X_s = a * r_s;
  const Eigen::Vector3d X_t = R * X_s + t;
  if (!X_s.allFinite() || !X_t.allFinite()) return out;
  out.ok = true;
  out.z_s = X_s.z();
  out.z_t = X_t.z();
  return out;
}

struct CheiralityWinner {
  bool ok = false;  // unique strict-majority winner found
  Eigen::Matrix3d R;
  Eigen::Vector3d t;  // unit
};

// Counts inlier points with positive depth in BOTH cameras per candidate;
// keeps the unique strict-majority winner, else fails closed.
CheiralityWinner VoteCheirality(const EssentialDecomposition& decomp,
                               const std::vector<Eigen::Vector3d>& rays_s,
                               const std::vector<Eigen::Vector3d>& rays_t,
                               const std::vector<bool>& inlier) {
  CheiralityWinner winner;
  const Eigen::Matrix3d candidates_R[2] = {decomp.R1, decomp.R2};
  const Eigen::Vector3d candidates_t[2] = {decomp.t, -decomp.t};

  std::size_t inlier_total = 0;
  for (const bool b : inlier)
    if (b) ++inlier_total;
  if (inlier_total == 0) return winner;

  std::size_t best_votes = 0;
  int best_k = -1;
  bool tie = false;
  for (int k = 0; k < 4; ++k) {
    const Eigen::Matrix3d& R = candidates_R[k / 2];
    const Eigen::Vector3d& t = candidates_t[k % 2];
    std::size_t votes = 0;
    for (std::size_t i = 0; i < inlier.size(); ++i) {
      if (!inlier[i]) continue;
      const Depths d = TriangulateUnitRays(rays_s[i], rays_t[i], R, t);
      if (d.ok && d.z_s > 0.0 && d.z_t > 0.0) ++votes;
    }
    if (votes > best_votes) {
      best_votes = votes;
      best_k = k;
      tie = false;
    } else if (votes == best_votes) {
      tie = true;
    }
  }

  // Unique strict majority required; anything else is ambiguous geometry.
  if (best_k < 0 || tie) return winner;
  if (best_votes * 2 <= inlier_total) return winner;

  winner.ok = true;
  winner.R = candidates_R[best_k / 2];
  winner.t = candidates_t[best_k % 2];
  return winner;
}

}  // namespace

GeometricVerificationResult EssentialGeometricVerifier::Verify(
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
  // (Mirrors FundamentalGeometricVerifier exactly.)
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

  // --- D-LC-06 temporal separation gate (mirrors fundamental). ---
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

  // --- Calibration gate: this provider REQUIRES both cameras (fail closed).
  // Absence is never a fallback to fundamental estimation. Unsupported
  // models / degenerate intrinsics throw typed errors from FromReconCamera
  // (fail-closed model selection BEFORE any computation); those propagate.
  if (!input.source_camera.has_value() || !input.target_camera.has_value()) {
    res.closure.status = "rejected";
    res.rejection_reason = "missing_calibration";
    return res;
  }
  const CameraModel src_model =
      CameraModel::FromReconCamera(*input.source_camera);
  const CameraModel tgt_model =
      CameraModel::FromReconCamera(*input.target_camera);
  const Eigen::Matrix3d Ks_inv =
      IntrinsicsMatrix(*input.source_camera).inverse();
  const Eigen::Matrix3d Kt_inv_T =
      IntrinsicsMatrix(*input.target_camera).inverse().transpose();

  // --- Calibrated normalization (Step 2B): pixels + normalized + rays. ---
  std::vector<Eigen::Vector3d> src_px, tgt_px, src_norm, tgt_norm;
  std::vector<Eigen::Vector3d> rays_s, rays_t;
  src_px.reserve(corr.size());
  tgt_px.reserve(corr.size());
  src_norm.reserve(corr.size());
  tgt_norm.reserve(corr.size());
  rays_s.reserve(corr.size());
  rays_t.reserve(corr.size());
  for (const auto& c : corr) {
    src_px.emplace_back(src.keypoints[c.source_index].x,
                        src.keypoints[c.source_index].y, 1.0);
    tgt_px.emplace_back(tgt.keypoints[c.target_index].x,
                        tgt.keypoints[c.target_index].y, 1.0);
    const Eigen::Vector3d xn_s =
        NormalizedPoint(src_model, src.keypoints[c.source_index]);
    const Eigen::Vector3d xn_t =
        NormalizedPoint(tgt_model, tgt.keypoints[c.target_index]);
    src_norm.push_back(xn_s);
    tgt_norm.push_back(xn_t);
    rays_s.push_back((xn_s / xn_s.norm()).eval());
    rays_t.push_back((xn_t / xn_t.norm()).eval());
  }
  for (const auto& r : rays_s)
    if (!r.allFinite()) {
      res.closure.status = "rejected";
      res.rejection_reason = "no_valid_model";
      return res;
    }
  for (const auto& r : rays_t)
    if (!r.allFinite()) {
      res.closure.status = "rejected";
      res.rejection_reason = "no_valid_model";
      return res;
    }

  // --- Robust essential estimation (Step 2C). ---
  const RansacOutcome ransac = RobustEssential(src_norm, tgt_norm, src_px,
                                              tgt_px, Ks_inv, Kt_inv_T, options);
  res.geometric_model = "essential";
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
  res.geometric_residual = ransac.mean_residual_px;

  // --- Quality gates (P3-impl-7b §10, mirror fundamental). ---
  const bool enough_inliers = ransac.inlier_count >= options.min_inliers;
  const bool enough_ratio =
      res.inlier_ratio >= options.min_inlier_ratio - 1e-9;
  const bool residual_ok =
      res.geometric_residual <= options.max_residual;

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

  // --- Decomposition (Step 2D) + cheirality vote (Step 2E). ---
  if (!ransac.E.allFinite() || ransac.E.norm() < 1e-12) {
    res.closure.status = "rejected";
    res.rejection_reason = "no_valid_model";
    return res;
  }
  const EssentialDecomposition decomp = DecomposeEssential(ransac.E);
  const CheiralityWinner winner =
      VoteCheirality(decomp, rays_s, rays_t, ransac.inlier);
  if (!winner.ok) {
    res.closure.status = "rejected";
    res.rejection_reason = "ambiguous_cheirality";
    return res;
  }

  // --- Canonical conversion (Step 2F): essential -> T_source_target. ---
  // The decomposition yields (R_ess, t_ess) in the EPIPOLAR convention
  //   X_t = R_ess * X_s + t_ess      (source coords -> target coords)
  // with t_ess expressed in C_t. The platform's canonical relative transform
  // is T_source_target = T_ws^-1 * T_wt (pose_graph_helpers.h:14-18, D-PG-04,
  // RelativePoseBetween), which is the INVERSE of that map:
  //   R_st = R_ess^T ;  t_st = -R_ess^T * t_ess   (expressed in C_s)
  // This is the form the frozen resolver scales (t_W = R_ws * t_hat) and the
  // form the GTSAM BetweenFactor consumes verbatim, so exporting (R_ess,
  // t_ess) directly would invert every loop constraint. Non-finite or
  // degenerate winners fail closed here, never downstream.
  if (!winner.R.allFinite() || !winner.t.allFinite()) {
    res.closure.status = "rejected";
    res.rejection_reason = "ambiguous_cheirality";
    return res;
  }
  const double t_norm = winner.t.norm();
  if (!(t_norm > 1e-9) || !std::isfinite(t_norm)) {
    res.closure.status = "rejected";
    res.rejection_reason = "degenerate_translation";
    return res;
  }
  const Eigen::Matrix3d R_st = winner.R.transpose();
  const Eigen::Vector3d t_st =
      -winner.R.transpose() * (winner.t / t_norm);
  const double t_st_norm = t_st.norm();
  if (!(t_st_norm > 1e-9) || !std::isfinite(t_st_norm) || !R_st.allFinite() ||
      !t_st.allFinite()) {
    res.closure.status = "rejected";
    res.rejection_reason = "degenerate_translation";
    return res;
  }
  const Eigen::Vector3d t_hat = t_st / t_st_norm;
  const spatial::core::geometry::Quaternion q =
      spatial::core::geometry::Quaternion::FromRotationMatrix(R_st)
          .Normalized();
  if (!std::isfinite(q.x()) || !std::isfinite(q.y()) ||
      !std::isfinite(q.z()) || !std::isfinite(q.w())) {
    res.closure.status = "rejected";
    res.rejection_reason = "ambiguous_cheirality";
    return res;
  }

  // --- Accepted (Step 2G): verified + UNIT pose, still non-metric. ---
  // has_relative_pose stays false and relative_position_xyz stays zero: no
  // metric edge can be built from this result until the frozen metric
  // resolver produces metres (P3.1 Step 1 separation).
  res.verified = true;
  res.closure.status = "accepted";
  res.has_relative_pose = false;  // unit direction is NOT metres (§7, §11)
  spatial::core::UnitRelativePose unit;
  unit.rotation_xyzw = {q.x(), q.y(), q.z(), q.w()};
  unit.translation_direction_xyz = {t_hat.x(), t_hat.y(), t_hat.z()};
  res.unit_relative_pose = unit;
  res.rejection_reason.clear();
  return res;
}

}  // namespace spatial::adapters::visual_geometry
