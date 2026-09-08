// P3-impl-8c COLMAP bundle-adjustment adapter tests (P2/P16; docs/architecture/
// P3-impl-8c-bundle-adjustment-readiness.md §5).
//
// Drives the REAL SeamAdapter implementation
// (ColmapBundleAdjustmentAdapter) against the colmap_probe_shim stand-in for
// the absent COLMAP binary: the seam writes the canonical v3 into
// sparse/0, runs `bundle_adjuster --input_path sparse/0 --output_path
// sparse_ba`, and parses the refined native model back into the v4 canonical
// document. The shim's bundle_adjuster passes cameras.bin + images.bin through
// verbatim (fixed intrinsics + poses) and REBUILDS points3D.bin with every
// point's x offset by +0.5 — so the tests prove the OUTPUT model (and not a
// stale copy of the input) is what gets parsed.
//
// Asserts (P13): fresh v4 UUIDv4 id (never the v3 id), status "succeeded",
// intrinsics reproduced byte-identically on the v4 document, frame_ids
// recovered by image name, trace/metrics finite + deterministic across runs,
// and the exact subprocess argv (paths, fixed-intrinsics pins, robust-loss
// scale, pinned seed). Fail-closed paths (P12): missing seed, non-zero exit,
// timeout, no output model, unresolved keypoints, and a config/toggle that
// sneaks intrinsics refinement in.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <Eigen/Geometry>

#include "adapters/colmap/colmap_bundle_adjustment_adapter.h"
#include "adapters/colmap/colmap_config.h"
#include "core/errors/project_error.h"
#include "core/geometry/reconstruction_optimizer.h"
#include "core/reconstruction/reconstruction.h"
#include "core/utils/uuid.h"
#include "tests/unit/fixtures/closed_square_reconstruction.h"

namespace spatial::adapters::colmap {
namespace {

using spatial::core::AdapterError;
using spatial::core::ErrorCode;
using spatial::core::ProjectError;
using spatial::core::ReconImage;
using spatial::core::ReconPoint3D;
using spatial::core::Reconstruction;
using spatial::core::ValidationError;
using spatial::core::geometry::BundleAdjustmentInput;
using spatial::core::geometry::BundleAdjustmentResult;
using spatial::core::geometry::ReprojectionObservation;
using spatial::test::BuildDefaultClosedSquareScene;
using spatial::test::ClosedSquareScene;

#ifndef SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE
#error SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE must be defined by the test build
#endif

std::filesystem::path UniqueWorkspace(const std::string& tag) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("spatial_ba_" + tag + "_" + std::to_string(std::time(nullptr)) + "_" +
       std::to_string(rand()));
  std::filesystem::create_directories(root);
  return root;
}

ColmapConfig BaConfig() {
  // Fixed intrinsics: every refine_* intrinsics toggle omitted (defaults
  // false). Seed pinned per the P9/D6 rule.
  return ColmapConfig::FromJson(
      R"({"enabled_stages":["bundle_adjuster"],)"
      R"("bundle_adjuster":{"loss_function":"SoftL1","max_num_iterations":50},)"
      R"("seed":"pinned-8c-adapter"})");
}

class ColmapBundleAdjustmentAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    scene_ = BuildDefaultClosedSquareScene();
    ASSERT_TRUE(scene_.SelfCheck());

    // v3 reconstruction: the fixture's drifted poses + TRUE-intrinsics
    // keypoints; cleaned provenance.
    v3_ = scene_.rec;
    v3_.provenance.backend.name = "colmap";
    v3_.provenance.backend.version = "v3";
    v3_.provenance.backend.adapter_version = "0.1.0";
    v3_.provenance.configuration_hash =
        "abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdef";

    // Resolve the fixture's per-frame keypoints into the frame_id -> FeatureArtifact
    // chain the seam requires (2D is never inline).
    for (std::size_t i = 0; i < scene_.keypoints.size(); ++i) {
      std::vector<std::array<double, 2>> pixels;
      pixels.reserve(scene_.keypoints[i].size());
      for (const auto& kp : scene_.keypoints[i]) {
        pixels.push_back({kp.x, kp.y});
      }
      keypoints_by_frame_.emplace(v3_.images[i].frame_id, std::move(pixels));
    }

    for (std::size_t i = 0; i < v3_.images.size(); ++i) {
      for (std::size_t c = 0; c < scene_.corner_xyz.size(); ++c) {
        ReprojectionObservation obs;
        obs.image_id = v3_.images[i].image_id;
        obs.point3d_id = v3_.points3D[c].point3d_id;
        obs.keypoint_2d = Eigen::Vector2d(scene_.keypoints[i][c].x,
                                          scene_.keypoints[i][c].y);
        observations_.push_back(obs);
      }
    }
  }

  BundleAdjustmentInput MakeInput(const std::string& seed = "pinned-8c-adapter") {
    BundleAdjustmentInput in;
    in.source = v3_;
    in.observations = observations_;
    in.random_seed = seed;
    return in;
  }

  std::string ReadArgs(const std::filesystem::path& workspace) {
    std::ifstream in(workspace / "logs" / "args.txt");
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    return text;
  }

  ClosedSquareScene scene_;
  Reconstruction v3_;
  std::map<std::string, std::vector<std::array<double, 2>>> keypoints_by_frame_;
  std::vector<ReprojectionObservation> observations_;
};

TEST_F(ColmapBundleAdjustmentAdapterTest, RefinedOutputBecomesCanonicalV4) {
  ColmapBundleAdjustmentAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, BaConfig(),
      [this](const std::string& frame_id) {
        const auto it = keypoints_by_frame_.find(frame_id);
        EXPECT_NE(it, keypoints_by_frame_.end());
        return it->second;
      },
      UniqueWorkspace("happy"), 120000);

  const BundleAdjustmentResult result = adapter.optimize(MakeInput());
  const Reconstruction& v4 = result.reconstruction;

  EXPECT_NE(v4.reconstruction_id, v3_.reconstruction_id);
  EXPECT_EQ(v4.status, "succeeded");
  EXPECT_EQ(v4.scene_id, v3_.scene_id);
  EXPECT_EQ(v4.session_ids, v3_.session_ids);
  EXPECT_EQ(v4.coordinate_frame, v3_.coordinate_frame);
  EXPECT_EQ(v4.provenance.backend.name, "spatial_bundle_adjuster");
  EXPECT_EQ(v4.provenance.backend.adapter_version, kColmapAdapterVersion);
  EXPECT_FALSE(v4.provenance.configuration_hash.empty());

  // Intrinsics are FIXED (P5/D3): reproduced exactly on the v4 document.
  ASSERT_EQ(v4.cameras.size(), v3_.cameras.size());
  for (std::size_t c = 0; c < v4.cameras.size(); ++c) {
    EXPECT_EQ(v4.cameras[c].camera_id, v3_.cameras[c].camera_id);
    EXPECT_EQ(v4.cameras[c].width, v3_.cameras[c].width);
    EXPECT_EQ(v4.cameras[c].height, v3_.cameras[c].height);
    EXPECT_EQ(v4.cameras[c].intrinsic_model, v3_.cameras[c].intrinsic_model);
    EXPECT_DOUBLE_EQ(v4.cameras[c].fx, v3_.cameras[c].fx);
    EXPECT_DOUBLE_EQ(v4.cameras[c].fy, v3_.cameras[c].fy);
    EXPECT_DOUBLE_EQ(v4.cameras[c].cx, v3_.cameras[c].cx);
    EXPECT_DOUBLE_EQ(v4.cameras[c].cy, v3_.cameras[c].cy);
    EXPECT_EQ(v4.cameras[c].distortion_model, v3_.cameras[c].distortion_model);
  }

  // All images survive with poses ~verbatim (shim copies images.bin; the
  // writer->reader quaternion/translation round-trip is exact to 1e-9) and
  // frame_ids recovered by image name.
  ASSERT_EQ(v4.images.size(), v3_.images.size());
  for (std::size_t i = 0; i < v4.images.size(); ++i) {
    EXPECT_EQ(v4.images[i].image_id, v3_.images[i].image_id);
    EXPECT_EQ(v4.images[i].camera_id, v3_.images[i].camera_id);
    EXPECT_EQ(v4.images[i].name, v3_.images[i].name);
    EXPECT_EQ(v4.images[i].frame_id, v3_.images[i].frame_id);
    EXPECT_TRUE(v4.images[i].detected);
    for (int k = 0; k < 4; ++k) {
      EXPECT_NEAR(v4.images[i].pose.rotation_xyzw[k],
                  v3_.images[i].pose.rotation_xyzw[k], 1e-9);
    }
    for (int k = 0; k < 3; ++k) {
      EXPECT_NEAR(v4.images[i].pose.translation_xyz[k],
                  v3_.images[i].pose.translation_xyz[k], 1e-9);
    }
  }

  // The refined points3D.bin WAS consumed: x is offset by +0.5 (shim), y/z
  // unchanged — proving the output, not the input, is what got parsed.
  ASSERT_EQ(v4.points3D.size(), v3_.points3D.size());
  for (std::size_t p = 0; p < v4.points3D.size(); ++p) {
    EXPECT_EQ(v4.points3D[p].point3d_id, v3_.points3D[p].point3d_id);
    EXPECT_NEAR(v4.points3D[p].xyz[0], v3_.points3D[p].xyz[0] + 0.5, 1e-9);
    EXPECT_NEAR(v4.points3D[p].xyz[1], v3_.points3D[p].xyz[1], 1e-9);
    EXPECT_NEAR(v4.points3D[p].xyz[2], v3_.points3D[p].xyz[2], 1e-9);
  }

  // Trace = D5 metrics of the SAME observation set over v3 (before) and v4
  // (after): finite, D5-compliant threshold, sensible counts.
  EXPECT_TRUE(result.trace.converged);
  EXPECT_GE(result.trace.iterations, 1);
  EXPECT_TRUE(std::isfinite(result.trace.rms_before_px));
  EXPECT_TRUE(std::isfinite(result.trace.rms_after_px));
  EXPECT_GE(result.trace.threshold_px_before, 2.0);  // D5 floor
  EXPECT_EQ(result.trace.inlier_count_before + result.trace.outlier_count_before,
            static_cast<std::int64_t>(observations_.size()));
  EXPECT_EQ(result.trace.inlier_count_after + result.trace.outlier_count_after,
            static_cast<std::int64_t>(observations_.size()));
  EXPECT_FALSE(v4.provenance.backend_specific_json.empty());
}

TEST_F(ColmapBundleAdjustmentAdapterTest, EqualInputsGiveIdenticalMetrics) {
  const std::filesystem::path ws1 = UniqueWorkspace("det1");
  const std::filesystem::path ws2 = UniqueWorkspace("det2");
  const auto source = [this](const std::string& frame_id) {
    const auto it = keypoints_by_frame_.find(frame_id);
    EXPECT_NE(it, keypoints_by_frame_.end());
    return it->second;
  };

  ColmapBundleAdjustmentAdapter a1(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                                   BaConfig(), source, ws1, 120000);
  ColmapBundleAdjustmentAdapter a2(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                                   BaConfig(), source, ws2, 120000);
  const BundleAdjustmentResult r1 = a1.optimize(MakeInput());
  const BundleAdjustmentResult r2 = a2.optimize(MakeInput());

  // Derived metrics + provenance bytes are identical for equal inputs (D6);
  // the fresh UUIDv4 identities necessarily differ (D-CRM-07).
  EXPECT_NE(r1.reconstruction.reconstruction_id,
            r2.reconstruction.reconstruction_id);
  EXPECT_EQ(r1.trace.rms_before_px, r2.trace.rms_before_px);
  EXPECT_EQ(r1.trace.rms_after_px, r2.trace.rms_after_px);
  EXPECT_EQ(r1.reconstruction.provenance.backend_specific_json,
            r2.reconstruction.provenance.backend_specific_json);
  EXPECT_EQ(r1.reconstruction.provenance.configuration_hash,
            r2.reconstruction.provenance.configuration_hash);
}

TEST_F(ColmapBundleAdjustmentAdapterTest, SubprocessArgvIsCanonical) {
  const std::filesystem::path ws = UniqueWorkspace("argv");
  ColmapBundleAdjustmentAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, BaConfig(),
      [this](const std::string& frame_id) {
        return keypoints_by_frame_.at(frame_id);
      },
      ws, 120000);
  (void)adapter.optimize(MakeInput());

  const std::string args = ReadArgs(ws);
  EXPECT_NE(std::string::npos, args.find("bundle_adjuster"));
  // Workspace-local paths, never CAS hashes. The adapter logs the 8.3 short
  // form of the temp prefix (space-safe argv), so assert the relative payload
  // segments rather than the exact long path.
  const std::string input_segment =
      (std::filesystem::path("sparse") / "0").string();
  EXPECT_NE(std::string::npos, args.find("--input_path"));
  EXPECT_NE(std::string::npos, args.find(input_segment));
  EXPECT_NE(std::string::npos, args.find("--output_path"));
  EXPECT_NE(std::string::npos, args.find("sparse_ba"));
  // Fixed-intrinsics pins (P5/D-8c-3).
  EXPECT_NE(std::string::npos, args.find("--BundleAdjustment.refine_focal_length|0"));
  EXPECT_NE(std::string::npos,
            args.find("--BundleAdjustment.refine_principal_point|0"));
  EXPECT_NE(std::string::npos,
            args.find("--BundleAdjustment.refine_extra_params|0"));
  EXPECT_NE(std::string::npos,
            args.find("--BundleAdjustment.refine_intrinsics|0"));
  // Deterministic loss scale (D5 threshold auto-rule passes through).
  EXPECT_NE(std::string::npos,
            args.find("--BundleAdjustment.robust_loss_scale"));
  // Pinned seed forwarded to the backend (D6).
  EXPECT_NE(std::string::npos, args.find("--random_seed|pinned-8c-adapter"));
}

TEST_F(ColmapBundleAdjustmentAdapterTest, MissingPinnedSeedFailsClosed) {
  ColmapBundleAdjustmentAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, BaConfig(),
      [this](const std::string& frame_id) {
        return keypoints_by_frame_.at(frame_id);
      },
      UniqueWorkspace("noseed"), 120000);
  BundleAdjustmentInput in = MakeInput();
  in.random_seed = std::nullopt;
  EXPECT_THROW(adapter.optimize(in), ValidationError);
}

TEST_F(ColmapBundleAdjustmentAdapterTest, IntrinsicsRefineConfigRejected) {
  // FromJson rejects any intrinsics-refine toggle (P5).
  EXPECT_THROW(ColmapConfig::FromJson(
                   R"({"enabled_stages":["bundle_adjuster"],)"
                   R"("bundle_adjuster":{"refine_focal_length":true},)"
                   R"("seed":"pinned-8c-adapter"})"),
               ValidationError);

  // The adapter constructor fails closed even if a config is constructed by
  // hand around the validation (defense in depth, D3).
  ColmapConfig config = BaConfig();
  config.bundle_adjuster.refine_intrinsics = true;
  EXPECT_THROW(
      ColmapBundleAdjustmentAdapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                                    config, [this](const std::string& frame_id) {
                                      return keypoints_by_frame_.at(frame_id);
                                    },
                                    UniqueWorkspace("refine"), 120000),
      ValidationError);
}

TEST_F(ColmapBundleAdjustmentAdapterTest, EmptyWorkspaceRejected) {
  ColmapConfig config = BaConfig();
  EXPECT_THROW(
      ColmapBundleAdjustmentAdapter(SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE,
                                    config, [this](const std::string& frame_id) {
                                      return keypoints_by_frame_.at(frame_id);
                                    },
                                    std::filesystem::path{}),
      ValidationError);
}

TEST_F(ColmapBundleAdjustmentAdapterTest, NonZeroExitFailsClosed) {
  const std::filesystem::path ws = UniqueWorkspace("fail");
  std::ofstream(ws / "shim_fail") << "marker";
  ColmapBundleAdjustmentAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, BaConfig(),
      [this](const std::string& frame_id) {
        return keypoints_by_frame_.at(frame_id);
      },
      ws, 120000);
  EXPECT_THROW(adapter.optimize(MakeInput()), AdapterError);
}

TEST_F(ColmapBundleAdjustmentAdapterTest, TimeoutFailsClosed) {
  const std::filesystem::path ws = UniqueWorkspace("hang");
  std::ofstream(ws / "shim_hang") << "marker";
  ColmapBundleAdjustmentAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, BaConfig(),
      [this](const std::string& frame_id) {
        return keypoints_by_frame_.at(frame_id);
      },
      ws, 1000);  // shim sleeps 30s -> must be terminated by the timeout
  EXPECT_THROW(adapter.optimize(MakeInput()), AdapterError);
}

TEST_F(ColmapBundleAdjustmentAdapterTest, NoOutputModelFailsClosed) {
  const std::filesystem::path ws = UniqueWorkspace("noout");
  std::ofstream(ws / "shim_bundle_adjuster_no_output") << "marker";
  ColmapBundleAdjustmentAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, BaConfig(),
      [this](const std::string& frame_id) {
        return keypoints_by_frame_.at(frame_id);
      },
      ws, 120000);
  EXPECT_THROW(adapter.optimize(MakeInput()), AdapterError);
}

TEST_F(ColmapBundleAdjustmentAdapterTest, UnresolvableFrameIdFailsClosed) {
  // A v3 image without a frame_id cannot feed the keypoint chain.
  Reconstruction broken = v3_;
  broken.images[0].frame_id.clear();
  ColmapBundleAdjustmentAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, BaConfig(),
      [this](const std::string& frame_id) {
        return keypoints_by_frame_.at(frame_id);
      },
      UniqueWorkspace("noframe"), 120000);
  BundleAdjustmentInput in = MakeInput();
  in.source = broken;
  EXPECT_THROW(adapter.optimize(in), AdapterError);
}

TEST_F(ColmapBundleAdjustmentAdapterTest, KeypointSourceThrowsFailsClosed) {
  // The frame_id -> FeatureArtifact callback failure is typed and never turns
  // into a partial result.
  ColmapBundleAdjustmentAdapter adapter(
      SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE, BaConfig(),
      [](const std::string&) -> std::vector<std::array<double, 2>> {
        throw ValidationError(ErrorCode::kValidationDomain,
                              "feature artifact missing for frame");
      },
      UniqueWorkspace("kpsrc"), 120000);
  EXPECT_THROW(adapter.optimize(MakeInput()), ProjectError);
}

}  // namespace
}  // namespace spatial::adapters::colmap