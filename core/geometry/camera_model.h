#pragma once

// Canonical camera projection / back-projection model (P3-impl-8a, §4.8/§4.9).
//
// This is the single source of truth for how the platform turns a 3D point into
// an image pixel and back. Only the first-class camera models from the
// normative P3-impl-8 table (§4.9) are implemented:
//   - pinhole                                            (intrinsic_model "pinhole")
//   - OpenCV radial-tangential                           (intrinsic "opencv")
//   - OpenCV fisheye, equidistant / Kannala-Brandt       (intrinsic "opencv_fisheye",
//                                                         COLMAP OPENCV_FISHEYE)
//   - FOV-division                                       (intrinsic "fov", Devernay-Faugeras)
// Every other model (omnidirectional, custom, opengl, ...) FAILS CLOSED at
// model-selection time with a typed validation error — a model is NEVER
// silently treated as pinhole or any other model (§4.9 answer B).
//
// Convention (§4.8): ReconImage.pose is T_reconstruction_camera (world-from-
// camera). A camera-frame point p_C is obtained from a reconstruction-frame
// point p_R via p_C = T_cr * p_R = T_rc.Inverse() * p_R (see se3.h). Projection
// requires w_C > 0 (cheirality). Back-projection returns a UNIT ray in the
// camera frame. The class is header-only and keeps raw Eigen strictly inside
// core/geometry/ (check_domain_types.py ALLOWED_DIRS).

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <Eigen/Geometry>

#include "core/errors/project_error.h"
#include "core/reconstruction/reconstruction.h"

namespace spatial::core::geometry {

// The concrete first-class camera model family (P3-impl-8a §4.9).
enum class CameraModelKind {
  kPinhole,
  kOpenCvRadial,
  kOpenCvFisheye,
  kFov,
};

// Canonical intrinsics. fx/fy must be positive and finite; cx/cy/width/height
// must be finite and width/height non-negative.
struct CameraIntrinsics {
  double fx = 0.0;
  double fy = 0.0;
  double cx = 0.0;
  double cy = 0.0;
  double width = 0.0;
  double height = 0.0;

  bool Valid() const {
    return std::isfinite(fx) && std::isfinite(fy) && std::isfinite(cx) &&
           std::isfinite(cy) && std::isfinite(width) &&
           std::isfinite(height) && fx > 0.0 && fy > 0.0 && width >= 0.0 &&
           height >= 0.0;
  }
};

// A validated, immutable camera model ready for projection. Construct one via
// the static factories (Pinhole / OpenCvRadial / OpenCvFisheye / Fov) or
// FromReconCamera / FromSpec (fail-closed model selection).
class CameraModel {
 public:
  CameraModelKind kind() const noexcept { return kind_; }
  const CameraIntrinsics& intrinsics() const noexcept { return intrinsics_; }

  // Factory: pinhole, no distortion.
  static CameraModel Pinhole(const CameraIntrinsics& intrinsics);
  // Factory: OpenCV radial-tangential. coefficients order (OpenCV storage
  // order, same as the COLMAP OPENCV/RADIAL models): [k1, k2, p1, p2, (k3)].
  // Either 4 (no k3) or 5 (with k3) coefficients are accepted.
  static CameraModel OpenCvRadial(const CameraIntrinsics& intrinsics,
                                  const std::vector<double>& coefficients);
  // Factory: OpenCV fisheye, equidistant / Kannala-Brandt (Kannala & Brandt
  // 2006), matching COLMAP OPENCV_FISHEYE. coefficients order: [k1, k2, k3, k4].
  static CameraModel OpenCvFisheye(const CameraIntrinsics& intrinsics,
                                   const std::vector<double>& coefficients);
  // Factory: FOV-division (Devernay-Faugeras FOV, COLMAP FOV). coefficients is
  // a single value [omega] (the FOV half-angle parameter).
  static CameraModel Fov(const CameraIntrinsics& intrinsics,
                         const std::vector<double>& coefficients);

  // Fail-closed model selection from canonical ReconCamera fields. Throws
  // spatial::core::ValidationError (ErrorCode::kValidationDomain) when the
  // intrinsic_model / distortion_model combination is NOT in the first-class
  // set, or spatial::core::CalibrationError (ErrorCode::kCalibrationInvalid)
  // when intrinsics are degenerate. Model selection always happens BEFORE any
  // computation (§4.9 answer B; no silent substitution).
  static CameraModel FromReconCamera(const ReconCamera& camera);

  // Projects a camera-frame 3D point to an image pixel (px). Throws
  // spatial::core::ValidationError if the point is non-finite or behind the
  // camera (w_C <= 0, cheirality §4.8).
  Eigen::Vector2d Project(const Eigen::Vector3d& p_c) const;

  // Back-projects an image pixel to a UNIT ray in the camera frame (depth 1,
  // §4.8). Throws spatial::core::ValidationError if the pixel is non-finite.
  Eigen::Vector3d Unproject(const Eigen::Vector2d& pixel) const;

  // Distortion-only maps acting on NORMALIZED coordinates:
  //   DistortUnit : undistorted normalized (x, y) -> distorted normalized.
  //   UndistortUnit: distorted normalized (x_d, y_d) -> undistorted normalized.
  // pinhole is the identity. opencv_radial undistortion is a fixed-point
  // inversion with a documented convergence tolerance (see UndistortUnit).
  Eigen::Vector2d DistortUnit(const Eigen::Vector2d& x) const;
  Eigen::Vector2d UndistortUnit(const Eigen::Vector2d& xd) const;

  // Pixel-space conveniences (compose intrinsics with the distortion maps).
  Eigen::Vector2d Distort(const Eigen::Vector2d& undistorted_pixel) const;
  Eigen::Vector2d Undistort(const Eigen::Vector2d& distorted_pixel) const;

 private:
  CameraModel(CameraModelKind kind, CameraIntrinsics intrinsics,
              std::vector<double> coefficients, double omega)
      : kind_(kind),
        intrinsics_(intrinsics),
        coefficients_(std::move(coefficients)),
        omega_(omega) {}

  static void ValidateIntrinsics(const CameraIntrinsics& intrinsics);

  CameraModelKind kind_;
  CameraIntrinsics intrinsics_;
  std::vector<double> coefficients_;
  double omega_ = 0.0;  // FOV model omega (half FOV, radians)
};

// ---------------------------------------------------------------------------
// Free helpers (pixel <-> normalized coordinate), reused by all models.
// ---------------------------------------------------------------------------

inline Eigen::Vector2d PixelToNormalized(const CameraIntrinsics& k,
                                         const Eigen::Vector2d& pixel) {
  return Eigen::Vector2d((pixel.x() - k.cx) / k.fx,
                         (pixel.y() - k.cy) / k.fy);
}

inline Eigen::Vector2d NormalizedToPixel(const CameraIntrinsics& k,
                                         const Eigen::Vector2d& x) {
  return Eigen::Vector2d(k.fx * x.x() + k.cx, k.fy * x.y() + k.cy);
}

// ---------------------------------------------------------------------------
// Inline implementations.
// ---------------------------------------------------------------------------

inline void CameraModel::ValidateIntrinsics(const CameraIntrinsics& i) {
  if (!i.Valid()) {
    throw CalibrationError(
        ErrorCode::kCalibrationInvalid,
        "camera model: intrinsics degenerate (fx/fy must be positive and "
        "finite; cx/cy/width/height must be finite and non-negative)");
  }
}

inline CameraModel CameraModel::Pinhole(const CameraIntrinsics& i) {
  ValidateIntrinsics(i);
  return CameraModel(CameraModelKind::kPinhole, i, {}, 0.0);
}

inline CameraModel CameraModel::OpenCvRadial(
    const CameraIntrinsics& i, const std::vector<double>& coefficients) {
  ValidateIntrinsics(i);
  if (coefficients.size() != 4u && coefficients.size() != 5u) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "camera model: opencv radial requires 4 or 5 coefficients "
        "[k1,k2,p1,p2,(k3)]");
  }
  for (const double c : coefficients) {
    if (!std::isfinite(c)) {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "camera model: non-finite distortion coefficient");
    }
  }
  return CameraModel(CameraModelKind::kOpenCvRadial, i, coefficients, 0.0);
}

inline CameraModel CameraModel::OpenCvFisheye(
    const CameraIntrinsics& i, const std::vector<double>& coefficients) {
  ValidateIntrinsics(i);
  if (coefficients.size() != 4u) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "camera model: opencv_fisheye requires 4 coefficients [k1,k2,k3,k4]");
  }
  for (const double c : coefficients) {
    if (!std::isfinite(c)) {
      throw ValidationError(ErrorCode::kValidationDomain,
                            "camera model: non-finite distortion coefficient");
    }
  }
  return CameraModel(CameraModelKind::kOpenCvFisheye, i, coefficients, 0.0);
}

inline CameraModel CameraModel::Fov(const CameraIntrinsics& i,
                                    const std::vector<double>& coefficients) {
  ValidateIntrinsics(i);
  if (coefficients.size() != 1u) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "camera model: fov requires exactly one coefficient [omega]");
  }
  if (!std::isfinite(coefficients[0])) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "camera model: non-finite fov omega");
  }
  return CameraModel(CameraModelKind::kFov, i, coefficients, coefficients[0]);
}

inline CameraModel CameraModel::FromReconCamera(const ReconCamera& camera) {
  const CameraIntrinsics intr{camera.fx, camera.fy, camera.cx, camera.cy,
                              static_cast<double>(camera.width),
                              static_cast<double>(camera.height)};

  // Fail-closed model selection (§4.9 answer B). A model outside the
  // first-class set is rejected here, BEFORE any computation, and is never
  // approximated as any other model.
  const std::string& im = camera.intrinsic_model;

  if (im == "pinhole") {
    if (camera.distortion_model != "none" && !camera.distortion_model.empty()) {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "camera model: pinhole requires distortion_model 'none' (got '" +
              camera.distortion_model +
              "'); unsupported combination fails closed");
    }
    return Pinhole(intr);
  }
  if (im == "opencv") {
    if (camera.distortion_model != "opencv_radial") {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "camera model: opencv requires distortion_model 'opencv_radial' "
          "(got '" +
              camera.distortion_model + "')");
    }
    return OpenCvRadial(intr, camera.distortion_coefficients);
  }
  if (im == "opencv_fisheye") {
    if (camera.distortion_model != "opencv_fisheye") {
      throw ValidationError(
          ErrorCode::kValidationDomain,
          "camera model: opencv_fisheye requires distortion_model "
          "'opencv_fisheye' (got '" +
              camera.distortion_model + "')");
    }
    return OpenCvFisheye(intr, camera.distortion_coefficients);
  }
  if (im == "fov") {
    // FOV-division is first-class; the distortion_model field is advisory
    // ("none" or "custom") and does not change the FOV-division math.
    return Fov(intr, camera.distortion_coefficients);
  }

  // omnidirectional, custom, opengl, empty, or any other intrinsic model:
  // unsupported in 8a -> fail closed.
  throw ValidationError(
      ErrorCode::kValidationDomain,
      "camera model: unsupported intrinsic_model '" + im +
          "' is not first-class in 8a (pinhole/opencv/opencv_fisheye/fov); "
          "the model fails closed and is never approximated as another model");
}

inline Eigen::Vector2d CameraModel::Project(const Eigen::Vector3d& p_c) const {
  if (!p_c.allFinite()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "camera model: cannot project non-finite point");
  }
  if (p_c.z() <= 0.0) {
    throw ValidationError(
        ErrorCode::kValidationDomain,
        "camera model: cannot project point with non-positive camera-frame "
        "depth (w_C <= 0 violates cheirality §4.8)");
  }
  const Eigen::Vector2d x_dist =
      DistortUnit(Eigen::Vector2d(p_c.x() / p_c.z(), p_c.y() / p_c.z()));
  return NormalizedToPixel(intrinsics_, x_dist);
}

inline Eigen::Vector3d CameraModel::Unproject(const Eigen::Vector2d& pixel) const {
  if (!pixel.allFinite()) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "camera model: cannot back-project non-finite pixel");
  }
  const Eigen::Vector2d x_undist =
      UndistortUnit(PixelToNormalized(intrinsics_, pixel));
  Eigen::Vector3d ray(x_undist.x(), x_undist.y(), 1.0);
  const double norm = ray.norm();
  if (norm < 1e-12 || !std::isfinite(norm)) {
    throw ValidationError(ErrorCode::kValidationDomain,
                          "camera model: degenerate back-projection ray");
  }
  return ray / norm;
}

inline Eigen::Vector2d CameraModel::Distort(const Eigen::Vector2d& pixel) const {
  return NormalizedToPixel(intrinsics_,
                           DistortUnit(PixelToNormalized(intrinsics_, pixel)));
}

inline Eigen::Vector2d CameraModel::Undistort(const Eigen::Vector2d& pixel) const {
  return NormalizedToPixel(intrinsics_,
                           UndistortUnit(PixelToNormalized(intrinsics_, pixel)));
}

// Distinct distortion application for a normalized undistorted coordinate.
inline Eigen::Vector2d CameraModel::DistortUnit(const Eigen::Vector2d& x) const {
  switch (kind_) {
    case CameraModelKind::kPinhole: {
      return x;
    }
    case CameraModelKind::kOpenCvRadial: {
      const double k1 = coefficients_[0];
      const double k2 = coefficients_[1];
      const double p1 = coefficients_[2];
      const double p2 = coefficients_[3];
      const double k3 = coefficients_.size() == 5u ? coefficients_[4] : 0.0;
      const double x2 = x.x() * x.x();
      const double y2 = x.y() * x.y();
      const double r2 = x2 + y2;
      const double r4 = r2 * r2;
      const double r6 = r4 * r2;
      const double radial = 1.0 + k1 * r2 + k2 * r4 + k3 * r6;
      const double xd = x.x() * radial + 2.0 * p1 * x.x() * x.y() +
                        p2 * (r2 + 2.0 * x2);
      const double yd = x.y() * radial +
                        2.0 * p2 * x.x() * x.y() + p1 * (r2 + 2.0 * y2);
      return Eigen::Vector2d(xd, yd);
    }
    case CameraModelKind::kOpenCvFisheye: {
      const double k1 = coefficients_[0];
      const double k2 = coefficients_[1];
      const double k3 = coefficients_[2];
      const double k4 = coefficients_[3];
      const double r = std::sqrt(x.x() * x.x() + x.y() * x.y());
      if (r < 1e-12) {
        return x;
      }
      const double theta = std::atan(r);
      const double theta2 = theta * theta;
      const double theta4 = theta2 * theta2;
      const double theta6 = theta4 * theta2;
      const double theta8 = theta4 * theta4;
      const double theta_d =
          theta * (1.0 + k1 * theta2 + k2 * theta4 + k3 * theta6 + k4 * theta8);
      const double scale = theta_d / r;
      return Eigen::Vector2d(x.x() * scale, x.y() * scale);
    }
    case CameraModelKind::kFov: {
      const double r_u = std::sqrt(x.x() * x.x() + x.y() * x.y());
      if (r_u < 1e-12) {
        return x;
      }
      const double factor =
          std::tan(r_u * omega_) / (2.0 * r_u * std::tan(0.5 * omega_));
      return Eigen::Vector2d(x.x() * factor, x.y() * factor);
    }
  }
  // Unreachable for a valid model (fail-closed construction prevents unknown
  // kinds).
  throw ValidationError(ErrorCode::kValidationDomain,
                        "camera model: unknown model kind");
}

// Inversion of the distortion map for a normalized DISTORTED coordinate.
// Fixed-point convergence tolerance: documented here and enforced by the
// round-trip tests (residual below 1e-10 normalized units -> << 1e-5 px for
// typical fx). pinhole and fov are closed-form; opencv_radial and
// opencv_fisheye iterate to the documented tolerance.
inline Eigen::Vector2d CameraModel::UndistortUnit(const Eigen::Vector2d& xd) const {
  switch (kind_) {
    case CameraModelKind::kPinhole: {
      return xd;
    }
    case CameraModelKind::kFov: {
      const double r_d = std::sqrt(xd.x() * xd.x() + xd.y() * xd.y());
      if (r_d < 1e-12) {
        return xd;
      }
      const double r_u =
          std::atan(2.0 * r_d * std::tan(0.5 * omega_)) / omega_;
      const double factor = r_u / r_d;
      return Eigen::Vector2d(xd.x() * factor, xd.y() * factor);
    }
    case CameraModelKind::kOpenCvRadial: {
      // Fixed-point inversion: iterate x_{n+1} = xd - D(x_n) until the
      // distortion residual is below the fixed-point tolerance (documented
      // convergence criterion, §4.9 answer B).
      constexpr int kMaxIterations = 100;
      constexpr double kTolerance = 1e-10;  // normalized-units convergence tol
      Eigen::Vector2d x = xd;
      for (int iter = 0; iter < kMaxIterations; ++iter) {
        const Eigen::Vector2d residual = DistortUnit(x) - xd;
        if (residual.norm() < kTolerance) {
          break;
        }
        x -= residual;
      }
      return x;
    }
    case CameraModelKind::kOpenCvFisheye: {
      // Kannala-Brandt inverse: solve theta_d = rd where
      //   theta_d = theta (1 + k1 th^2 + k2 th^4 + k3 th^6 + k4 th^8)
      // via Newton's method on theta, then recover r = tan(theta). Exact for
      // the first-class model; converges to the documented tolerance.
      const double rd = std::sqrt(xd.x() * xd.x() + xd.y() * xd.y());
      if (rd < 1e-12) {
        return xd;
      }
      const double k1 = coefficients_[0];
      const double k2 = coefficients_[1];
      const double k3 = coefficients_[2];
      const double k4 = coefficients_[3];
      constexpr int kMaxIterations = 50;
      constexpr double kTol = 1e-12;  // radians
      double theta = rd;              // Newton start (theta ~ rd for small dist)
      for (int iter = 0; iter < kMaxIterations; ++iter) {
        const double th2 = theta * theta;
        const double th4 = th2 * th2;
        const double th6 = th4 * th2;
        const double th8 = th4 * th4;
        const double f = theta * (1.0 + k1 * th2 + k2 * th4 + k3 * th6 +
                                  k4 * th8) -
                         rd;
        const double df =
            1.0 + 3.0 * k1 * th2 + 5.0 * k2 * th4 + 7.0 * k3 * th6 +
            9.0 * k4 * th8;
        const double step = f / df;
        theta -= step;
        if (std::abs(step) < kTol) {
          break;
        }
      }
      const double r = std::tan(theta);
      const double factor = r / rd;
      return Eigen::Vector2d(xd.x() * factor, xd.y() * factor);
    }
  }
  throw ValidationError(ErrorCode::kValidationDomain,
                        "camera model: unknown model kind");
}

}  // namespace spatial::core::geometry
