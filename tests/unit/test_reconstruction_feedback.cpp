#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <Eigen/Geometry>
#include <nlohmann/json.hpp>

#include "core/geometry/se3.h"
#include "core/reconstruction/reconstruction.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/optimization.h"
#include "core/trajectory/pose_graph_helpers.h"
#include "core/trajectory/reconstruction_feedback.h"
#include "core/trajectory/trajectory.h"
#include "core/utils/sha256.h"
#include "core/utils/uuid.h"

namespace spatial::core {
namespace {

using geometry::Quaternion;
using geometry::SE3;
using nlohmann::json;

// Test-only helper: builds an Eigen translation (tests/ is outside the
// check_domain_types scan root, so raw Eigen tokens are permitted here).
inline Eigen::Vector3d EigenVector3(double x, double y, double z) {
  return Eigen::Vector3d(x, y, z);
}

// ---------------------------------------------------------------------------
// Test fixtures / helpers
// ---------------------------------------------------------------------------

// Builds a populated source Reconstruction (COLMAP-style v1) for the seam.
Reconstruction MakeSourceRecon() {
  Reconstruction r;
  r.reconstruction_id = FormatUuid(GenerateUuid());
  r.scene_id = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
  r.session_ids.push_back("6ba7b811-9dad-11d1-80b4-00c04fd430c8");
  r.coordinate_frame = "reconstruction_0";
  r.status = "succeeded";
  r.created_at_ns = 1000;
  r.provenance.backend.name = "colmap";
  r.provenance.backend.version = "3.13";
  r.provenance.backend.adapter_version = "1.0";

  ReconCamera cam;
  cam.camera_id = 1;
  cam.width = 640;
  cam.height = 480;
  cam.intrinsic_model = "pinhole";
  cam.fx = 500.0;
  cam.fy = 500.0;
  cam.cx = 320.0;
  cam.cy = 240.0;
  cam.distortion_model = "none";
  r.cameras.push_back(cam);

  // Three detected images with frame_ids A, B, C and one undetected (D).
  const char* ids[4] = {
      "11111111-1111-4111-8111-111111111111",
      "22222222-2222-4222-8222-222222222222",
      "33333333-3333-4333-8333-333333333333",
      "44444444-4444-4444-8444-444444444444"};
  for (int i = 0; i < 4; ++i) {
    ReconImage img;
    img.image_id = static_cast<std::uint32_t>(i + 1);
    img.camera_id = 1;
    img.frame_id = ids[i];
    img.name = "frame" + std::to_string(i) + ".jpg";
    img.detected = i < 3;  // D (index 3) undetected
    img.pose.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
    img.pose.translation_xyz = {static_cast<double>(i), 0.0, 0.0};
    r.images.push_back(img);
  }

  ReconPoint3D pt;
  pt.point3d_id = 100;
  pt.xyz = {1.0, 2.0, 3.0};
  pt.color = {255, 128, 0};
  pt.error = 0.5;
  pt.track.push_back({.image_id = 1, .point2d_idx = 0});
  pt.track.push_back({.image_id = 2, .point2d_idx = 3});
  r.points3D.push_back(pt);
  return r;
}

// Builds a source Trajectory with nodes A, B, C, D.
Trajectory MakeSourceTrajectory() {
  Trajectory t;
  t.trajectory_id = FormatUuid(GenerateUuid());
  t.scene_id = "6ba7b810-9dad-11d1-80b4-00c04fd430c8";
  t.session_id = "6ba7b811-9dad-11d1-80b4-00c04fd430c8";
  t.kind = "sfm";
  t.coordinate_frame = "trajectory_0";
  t.status = "optimized";
  t.created_at_ns = 1500;
  return t;
}

TrajectoryPoseNode MakeTrajNode(int idx) {
  TrajectoryPoseNode n;
  n.sequence_index = idx;
  switch (idx) {
    case 0:
      n.frame_id = "11111111-1111-4111-8111-111111111111";
      break;
    case 1:
      n.frame_id = "22222222-2222-4222-8222-222222222222";
      break;
    case 2:
      n.frame_id = "33333333-3333-4333-8333-333333333333";
      break;
    default:
      n.frame_id = "44444444-4444-4444-8444-444444444444";
      break;
  }
  n.position_xyz = {static_cast<double>(idx), 0.0, 0.0};
  n.rotation_xyzw = {0.0, 0.0, 0.0, 1.0};
  return n;
}

// Corrected nodes: translate frame i's pose so the new T_trajectory_camera
// is a known, non-trivial value. For identity-frame tests this must match the
// poses the Reconstruction is expected to adopt.
OptimizedPoseNode MakeOptimizedNode(int idx) {
  OptimizedPoseNode n;
  n.sequence_index = idx;
  n.timestamp_ns = 10 * idx;
  switch (idx) {
    case 0:
      n.frame_id = "11111111-1111-4111-8111-111111111111";
      break;
    case 1:
      n.frame_id = "22222222-2222-4222-8222-222222222222";
      break;
    case 2:
      n.frame_id = "33333333-3333-4333-8333-333333333333";
      break;
    default:
      n.frame_id = "44444444-4444-4444-8444-444444444444";
      break;
  }
  // A 90-degree rotation about Z plus a translation.
  n.position_xyz = {10.0 * idx, 5.0, -2.0};
  n.rotation_xyzw = {0.0, 0.0, 0.7071067811865476, 0.7071067811865476};
  return n;
}

OptimizationResult MakeOptResult() {
  OptimizationResult o;
  o.result_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";  // fixed, deterministic
  o.status = "converged";
  o.iterations = 12;
  o.created_at_ns = 2000;
  o.provenance.optimizer.name = "gtsam";
  o.provenance.optimizer.version = "4.2.0";
  o.provenance.adapter_version = "2.0";
  o.provenance.input_artifact_hashes = {"posegraph-hash"};
  return o;
}

// Deterministic canonical serialization of a Reconstruction (v2 document shape,
// matching ReconstructionToJson / reconstruction.schema.json) for CAS
// determinism and schema-validation assertions.
std::string CanonicalJson(const Reconstruction& r) {
  // Test-local serializer; proves identical seam content yields identical
  // content-addressed bytes and a schema-conformant shape.
  json doc;
  doc["schema_version"] = 2;
  doc["reconstruction_id"] = r.reconstruction_id;
  doc["scene_id"] = r.scene_id;
  json sids = json::array();
  for (const auto& s : r.session_ids) sids.push_back(s);
  doc["session_ids"] = std::move(sids);
  doc["coordinate_frame"] = r.coordinate_frame;
  if (!r.status.empty()) doc["status"] = r.status;
  doc["created_at_ns"] = r.created_at_ns;

  json prov;
  json backend;
  backend["name"] = r.provenance.backend.name;
  backend["version"] = r.provenance.backend.version;
  backend["adapter_version"] = r.provenance.backend.adapter_version;
  prov["backend"] = std::move(backend);
  prov["configuration_hash"] = r.provenance.configuration_hash;
  json iah = json::array();
  for (const auto& h : r.provenance.input_artifact_hashes) iah.push_back(h);
  prov["input_artifact_hashes"] = std::move(iah);
  json bsj = json::object();
  if (!r.provenance.backend_specific_json.empty()) {
    try {
      bsj = json::parse(r.provenance.backend_specific_json);
    } catch (...) {
      bsj = r.provenance.backend_specific_json;
    }
  }
  prov["backend_specific_json"] = std::move(bsj);
  doc["provenance"] = std::move(prov);

  json cams = json::array();
  for (const auto& c : r.cameras) {
    json e;
    e["camera_id"] = c.camera_id;
    e["width"] = c.width;
    e["height"] = c.height;
    e["intrinsic_model"] = c.intrinsic_model;
    e["fx"] = c.fx;
    e["fy"] = c.fy;
    e["cx"] = c.cx;
    e["cy"] = c.cy;
    e["distortion_model"] = c.distortion_model;
    e["distortion_coefficients"] = c.distortion_coefficients;
    cams.push_back(std::move(e));
  }
  doc["cameras"] = std::move(cams);

  json imgs = json::array();
  for (const auto& img : r.images) {
    json e;
    e["image_id"] = img.image_id;
    e["camera_id"] = img.camera_id;
    if (!img.frame_id.empty()) e["frame_id"] = img.frame_id;
    e["name"] = img.name;
    json pose;
    pose["rotation_xyzw"] = {img.pose.rotation_xyzw[0], img.pose.rotation_xyzw[1],
                             img.pose.rotation_xyzw[2], img.pose.rotation_xyzw[3]};
    pose["translation_xyz"] = {img.pose.translation_xyz[0],
                               img.pose.translation_xyz[1],
                               img.pose.translation_xyz[2]};
    e["pose"] = std::move(pose);
    e["detected"] = img.detected;
    imgs.push_back(std::move(e));
  }
  doc["images"] = std::move(imgs);

  json pts = json::array();
  for (const auto& p : r.points3D) {
    json e;
    e["point3d_id"] = p.point3d_id;
    e["xyz"] = {p.xyz[0], p.xyz[1], p.xyz[2]};
    e["color"] = {p.color[0], p.color[1], p.color[2]};
    e["error"] = p.error;
    json track = json::array();
    for (const auto& t : p.track) {
      track.push_back({{"image_id", t.image_id}, {"point2d_idx", t.point2d_idx}});
    }
    e["track"] = std::move(track);
    pts.push_back(std::move(e));
  }
  doc["points3D"] = std::move(pts);
  return doc.dump();
}

ReconstructionFeedbackInput MakeInput(const Reconstruction& source) {
  ReconstructionFeedbackInput in;
  in.source = source;
  in.trajectory = MakeSourceTrajectory();
  in.optimization_result = MakeOptResult();
  for (int i = 0; i < 3; ++i)
    in.trajectory_nodes.push_back(MakeTrajNode(i));
  for (int i = 0; i < 3; ++i)
    in.optimized_nodes.push_back(MakeOptimizedNode(i));
  in.reconstruction_from_trajectory = SE3::Identity();
  in.alignment_resolved = true;
  return in;
}

// ---------------------------------------------------------------------------
// 1. Join: both trajectory node and ReconImage present
// ---------------------------------------------------------------------------
TEST(Feedback, JoinBothPresent) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  const auto res = ApplyOptimizedTrajectory(in);
  const auto& r = res.reconstruction;

  // Poses for detected images A/B/C must be updated; order preserved.
  ASSERT_EQ(r.images.size(), src.images.size());
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(r.images[i].detected);
    EXPECT_EQ(r.images[i].frame_id, src.images[i].frame_id);
    const OptimizedPoseNode& on = in.optimized_nodes[i];
    // Identity alignment -> raw T_trajectory_camera write-back.
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[0], on.position_xyz[0]);
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[1], on.position_xyz[1]);
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[2], on.position_xyz[2]);
    EXPECT_DOUBLE_EQ(r.images[i].pose.rotation_xyzw[0], on.rotation_xyzw[0]);
    EXPECT_DOUBLE_EQ(r.images[i].pose.rotation_xyzw[3], on.rotation_xyzw[3]);
  }
}

// ---------------------------------------------------------------------------
// 2. Join: trajectory node with no matching ReconImage -> skipped, no phantom
// ---------------------------------------------------------------------------
TEST(Feedback, JoinTrajNoImage) {
  Reconstruction src = MakeSourceRecon();
  // Keep only image A (frame 1111...). B/C have trajectory nodes but no image.
  src.images.erase(src.images.begin() + 1, src.images.begin() + 4);
  auto in = MakeInput(src);
  const auto res = ApplyOptimizedTrajectory(in);
  EXPECT_EQ(res.reconstruction.images.size(), src.images.size());
  for (const auto& img : res.reconstruction.images)
    EXPECT_EQ(img.frame_id, "11111111-1111-4111-8111-111111111111");
}

// ---------------------------------------------------------------------------
// 3. Join: ReconImage with no matching trajectory -> original pose preserved
// ---------------------------------------------------------------------------
TEST(Feedback, JoinImageNoTraj) {
  Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  // Only node A exists; B/C images have no optimized node.
  in.optimized_nodes = {MakeOptimizedNode(0)};
  // Give A an optimized translation distinct from its original.
  in.optimized_nodes[0].position_xyz = {99.0, 0.0, 0.0};
  const auto res = ApplyOptimizedTrajectory(in);

  // A updated; B and C (detected, no node) preserved verbatim.
  EXPECT_DOUBLE_EQ(res.reconstruction.images[0].pose.translation_xyz[0], 99.0);
  for (int i = 1; i < 3; ++i) {
    EXPECT_EQ(res.reconstruction.images[i].pose, src.images[i].pose);
  }
}

// ---------------------------------------------------------------------------
// 4. Join: detected=false image copied unchanged, never updated
// ---------------------------------------------------------------------------
TEST(Feedback, JoinDetectedFalse) {
  Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  const auto res = ApplyOptimizedTrajectory(in);
  // Image D (index 3) is detected=false; must be byte-identical to source.
  EXPECT_FALSE(res.reconstruction.images[3].detected);
  EXPECT_EQ(res.reconstruction.images[3].pose, src.images[3].pose);
  EXPECT_EQ(res.reconstruction.images[3].frame_id, src.images[3].frame_id);
}

// ---------------------------------------------------------------------------
// 5. Coordinate-frame mismatch: no alignment -> fail closed, nothing written
// ---------------------------------------------------------------------------
TEST(Feedback, CoordinateFrameMismatch) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  in.alignment_resolved = false;  // frames differ, no alignment available
  EXPECT_THROW(ApplyOptimizedTrajectory(in), std::invalid_argument);
  // Source untouched (implicitly: the throw precedes any mutation).
  EXPECT_EQ(src.images[0].pose, MakeSourceRecon().images[0].pose);
}

// ---------------------------------------------------------------------------
// 6. Coordinate-frame identity: same frame (identity alignment) -> raw write
// ---------------------------------------------------------------------------
TEST(Feedback, CoordinateFrameIdentity) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  in.reconstruction_from_trajectory = SE3::Identity();
  const auto r = ApplyOptimizedTrajectory(in).reconstruction;
  for (int i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[0],
                     in.optimized_nodes[i].position_xyz[0]);
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[1],
                     in.optimized_nodes[i].position_xyz[1]);
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[2],
                     in.optimized_nodes[i].position_xyz[2]);
  }
}

// ---------------------------------------------------------------------------
// 7. Coordinate-frame aligned: T_rc = T_rt * T_tc
// ---------------------------------------------------------------------------
TEST(Feedback, CoordinateFrameAligned) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  // A known 90-deg rotation about Z firing from trajectory into reconstruction.
  SE3 T_rt(Quaternion(0.0, 0.0, 0.7071067811865476, 0.7071067811865476),
           EigenVector3(1.0, 2.0, 3.0));
  in.reconstruction_from_trajectory = T_rt;
  const auto r = ApplyOptimizedTrajectory(in).reconstruction;
  for (int i = 0; i < 3; ++i) {
    const SE3 T_tc = MakeCameraPose(in.optimized_nodes[i].position_xyz,
                                    in.optimized_nodes[i].rotation_xyzw);
    const SE3 T_rc_expected = T_rt * T_tc;
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[0],
                     T_rc_expected.translation().x());
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[1],
                     T_rc_expected.translation().y());
    EXPECT_DOUBLE_EQ(r.images[i].pose.translation_xyz[2],
                     T_rc_expected.translation().z());
    const auto q = T_rc_expected.rotation();
    EXPECT_DOUBLE_EQ(r.images[i].pose.rotation_xyzw[0], q.x());
    EXPECT_DOUBLE_EQ(r.images[i].pose.rotation_xyzw[3], q.w());
  }
}

// ---------------------------------------------------------------------------
// 8. NoDoubleInversion: golden numeric check on a non-trivial rotation
// ---------------------------------------------------------------------------
TEST(Feedback, NoDoubleInversion) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  const SE3 T_rt(Quaternion(0.0, 0.0, 0.7071067811865476, 0.7071067811865476),
                 EigenVector3(2.0, -1.0, 0.5));
  in.reconstruction_from_trajectory = T_rt;
  in.optimized_nodes = {MakeOptimizedNode(0)};
  const auto node = in.optimized_nodes[0];
  const SE3 T_tc = MakeCameraPose(node.position_xyz, node.rotation_xyzw);

  const auto r = ApplyOptimizedTrajectory(in).reconstruction;
  const SE3 T_rc_got = MakeCameraPose(r.images[0].pose.translation_xyz,
                                      r.images[0].pose.rotation_xyzw);

  // Must equal T_rt * T_tc (NOT T_rt * T_tc^-1, NOT T_rt^-1 * T_tc).
  const SE3 correct = T_rt * T_tc;
  const SE3 wrong1 = T_rt * T_tc.Inverse();
  const SE3 wrong2 = T_rt.Inverse() * T_tc;
  const auto close = [](const SE3& a, const SE3& b) {
    const double dq = std::abs(a.rotation().w() - b.rotation().w()) +
                      std::abs(a.rotation().x() - b.rotation().x());
    const double dt = (a.translation() - b.translation()).norm();
    return dq < 1e-9 && dt < 1e-9;
  };
  EXPECT_TRUE(close(T_rc_got, correct)) << "must equal T_rt * T_tc";
  EXPECT_FALSE(close(T_rc_got, wrong1)) << "must NOT be T_rt * T_tc^-1";
  EXPECT_FALSE(close(T_rc_got, wrong2)) << "must NOT be T_rt^-1 * T_tc";
}

// ---------------------------------------------------------------------------
// 9. UnmatchedNodes: matched updated, unmatched preserved, order stable
// ---------------------------------------------------------------------------
TEST(Feedback, UnmatchedNodes) {
  Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  // Remove the trajectory node for B; keep C node. B image preserved.
  in.optimized_nodes = {MakeOptimizedNode(0), MakeOptimizedNode(2)};
  const auto r = ApplyOptimizedTrajectory(in).reconstruction;
  // A and C updated; B preserved.
  EXPECT_NE(r.images[0].pose, src.images[0].pose);
  EXPECT_EQ(r.images[1].pose, src.images[1].pose);
  EXPECT_NE(r.images[2].pose, src.images[2].pose);
  // Ordering: output images list matches source order (1..4).
  for (std::size_t i = 0; i < r.images.size(); ++i)
    EXPECT_EQ(r.images[i].image_id, src.images[i].image_id);
}

// ---------------------------------------------------------------------------
// 10. MissingFrameId: empty on image (skip/keep) and node (skip); continues
// ---------------------------------------------------------------------------
TEST(Feedback, MissingFrameId) {
  Reconstruction src = MakeSourceRecon();
  // Image C has an empty frame_id -> must keep original pose, not crash.
  src.images[2].frame_id = "";
  auto in = MakeInput(src);
  // A node with empty frame_id -> skipped.
  OptimizedPoseNode empty_node = MakeOptimizedNode(0);
  empty_node.frame_id = "";
  in.optimized_nodes.emplace_back(std::move(empty_node));

  std::vector<PoseFeedbackDetail> details;
  const auto r = ApplyOptimizedTrajectory(in, &details).reconstruction;
  ASSERT_EQ(details.size(), r.images.size());
  // Image with empty frame_id: skipped + preserved.
  EXPECT_TRUE(details[2].skipped);
  EXPECT_TRUE(details[2].preserved);
  EXPECT_EQ(r.images[2].pose, src.images[2].pose);  // original kept
}

// ---------------------------------------------------------------------------
// 11. DeterministicOutput: identical inputs -> identical content (modulo id)
// ---------------------------------------------------------------------------
TEST(Feedback, DeterministicOutput) {
  const Reconstruction src = MakeSourceRecon();
  auto in1 = MakeInput(src);
  auto in2 = MakeInput(src);
  const auto r1 = ApplyOptimizedTrajectory(in1).reconstruction;
  const auto r2 = ApplyOptimizedTrajectory(in2).reconstruction;

  // Only the instance UUID may differ (D-DI-01). Normalize and compare.
  Reconstruction a = r1;
  Reconstruction b = r2;
  a.reconstruction_id = b.reconstruction_id;
  EXPECT_EQ(a, b);

  // Poses are content-deterministic regardless of the instance UUID.
  for (std::size_t i = 0; i < r1.images.size(); ++i)
    EXPECT_EQ(r1.images[i].pose, r2.images[i].pose);
}

// ---------------------------------------------------------------------------
// 12. CasHashDeterminism: identical inputs -> identical content SHA-256
// ---------------------------------------------------------------------------
TEST(Feedback, CasHashDeterminism) {
  const Reconstruction src = MakeSourceRecon();
  auto in1 = MakeInput(src);
  auto in2 = MakeInput(src);
  auto r1 = ApplyOptimizedTrajectory(in1).reconstruction;
  auto r2 = ApplyOptimizedTrajectory(in2).reconstruction;
  // CAS content-addressing excludes the instance UUID: normalize it so the
  // payload bytes are identical (Mapping §11.1).
  r1.reconstruction_id = r2.reconstruction_id;

  const std::string doc1 = CanonicalJson(r1);
  const std::string doc2 = CanonicalJson(r2);
  EXPECT_EQ(doc1, doc2);
  const std::string h1 = Sha256Hex(doc1);
  const std::string h2 = Sha256Hex(doc2);
  EXPECT_EQ(h1, h2);
  // Identical content yields the same content address (dedupe property).
  EXPECT_EQ(h1, Sha256Hex(doc2));
  EXPECT_EQ(h1.size(), 64u);
}

// ---------------------------------------------------------------------------
// 13. SchemaValidation: produced doc has required fields, status="succeeded"
// ---------------------------------------------------------------------------
TEST(Feedback, SchemaValidation) {
  std::ifstream in(SPATIAL_RECONSTRUCTION_SCHEMA_JSON);
  ASSERT_TRUE(in.good()) << "cannot open reconstruction.schema.json";
  const json schema =
      json::parse(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

  const Reconstruction src = MakeSourceRecon();
  const auto r = ApplyOptimizedTrajectory(MakeInput(src)).reconstruction;

  // Root "required" list must all be present in the produced document.
  const json doc = json::parse(CanonicalJson(r));
  ASSERT_TRUE(schema.contains("required")) << "schema has no root required";
  for (const auto& key : schema["required"]) {
    EXPECT_TRUE(doc.contains(key.get<std::string>()))
        << "missing required '" << key.get<std::string>() << "'";
  }
  EXPECT_EQ(r.status, "succeeded");
  EXPECT_TRUE(ValidateOptimizedReconstruction(r));
  // Required fields non-empty (identity).
  EXPECT_FALSE(r.reconstruction_id.empty());
  EXPECT_FALSE(r.scene_id.empty());
  EXPECT_FALSE(r.coordinate_frame.empty());
}

// ---------------------------------------------------------------------------
// 14. OptionAPointsUnchanged: points/cameras/detected byte-identical
// ---------------------------------------------------------------------------
TEST(Feedback, OptionAPointsUnchanged) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  in.reconstruction_from_trajectory =
      SE3(Quaternion(0.0, 0.0, 0.7071067811865476, 0.7071067811865476),
          EigenVector3(5.0, -3.0, 2.0));
  const auto r = ApplyOptimizedTrajectory(in).reconstruction;

  EXPECT_EQ(r.points3D, src.points3D);        // xyz + track untouched
  EXPECT_EQ(r.points3D[0].xyz, src.points3D[0].xyz);
  EXPECT_EQ(r.points3D[0].track, src.points3D[0].track);
  EXPECT_EQ(r.cameras, src.cameras);          // intrinsics untouched
  EXPECT_EQ(r.images[3].detected, src.images[3].detected);
  EXPECT_EQ(r.session_ids, src.session_ids);  // scene/session preserved
  EXPECT_EQ(r.scene_id, src.scene_id);
  // coordinate_frame preserved (Mapping §6).
  EXPECT_EQ(r.coordinate_frame, src.coordinate_frame);
  // Only matched, detected poses changed.
  EXPECT_NE(r.images[0].pose, src.images[0].pose);
  EXPECT_EQ(r.images[3].pose, src.images[3].pose);  // undetected kept
}

// ---------------------------------------------------------------------------
// 15. RevisionSemantics (DB): new id, supersede old row, current row succeeded
// ---------------------------------------------------------------------------
class FeedbackDbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_fb_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    path_ = root_ / "project.db";
    db_ = MetadataDb::Create(path_);
    project_id_ = GenerateUuid();
    db_.InsertProject(project_id_, "fb_project", 1, "{}", 1000, "ENU", "world",
                      "{}", "{}");
    scene_ = db_.FindOrCreateScene(project_id_, "fb_scene", "{}", 2000);
  }
  void TearDown() override {
    db_.Close();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }
  std::filesystem::path root_;
  std::filesystem::path path_;
  MetadataDb db_;
  Uuid project_id_{};
  SceneRow scene_{};
};

TEST_F(FeedbackDbTest, RevisionSemantics) {
  const Reconstruction src = MakeSourceRecon();
  // Seed the source row as the active "succeeded" reconstruction.
  ReconstructionRow old_row;
  old_row.reconstruction_id = ParseUuid(src.reconstruction_id);
  old_row.scene_id = scene_.scene_id;
  old_row.coordinate_frame = src.coordinate_frame;
  old_row.status = "succeeded";
  old_row.created_at_ns = 1000;
  old_row.document_json = "{}";
  db_.AddReconstruction(old_row);

  auto in = MakeInput(src);
  in.source.reconstruction_id = src.reconstruction_id;
  const auto v2 = ApplyOptimizedTrajectory(in).reconstruction;

  // New instance id, distinct from source.
  EXPECT_NE(v2.reconstruction_id, src.reconstruction_id);
  EXPECT_EQ(v2.status, "succeeded");

  // Persist: insert new row, supersede old row.
  ReconstructionRow new_row;
  new_row.reconstruction_id = ParseUuid(v2.reconstruction_id);
  new_row.scene_id = scene_.scene_id;
  new_row.coordinate_frame = v2.coordinate_frame;
  new_row.status = v2.status;
  new_row.created_at_ns = v2.created_at_ns;
  new_row.document_json = CanonicalJson(v2);
  db_.AddReconstruction(new_row);
  db_.SetReconstructionStatus(old_row.reconstruction_id, "superseded");

  // New row is now the active reconstruction; old one is superseded.
  const auto latest = db_.QueryLatestReconstructionByScene(scene_.scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->reconstruction_id, new_row.reconstruction_id);

  const auto all = db_.FindReconstructionsByScene(scene_.scene_id);
  ASSERT_EQ(all.size(), 2u);
  bool saw_superseded = false;
  for (const auto& row : all)
    if (row.reconstruction_id == old_row.reconstruction_id)
      saw_superseded = (row.status == "superseded");
  EXPECT_TRUE(saw_superseded);

  // Source document itself is immutable: this seam never mutated it.
  EXPECT_EQ(src.reconstruction_id, FormatUuid(old_row.reconstruction_id));
}

TEST_F(FeedbackDbTest, RevisionSemanticsSucceedsInSameScene) {
  // Same-scene supersede leaves exactly one active reconstruction.
  const Reconstruction src = MakeSourceRecon();
  ReconstructionRow old_row;
  old_row.reconstruction_id = ParseUuid(src.reconstruction_id);
  old_row.scene_id = scene_.scene_id;
  old_row.coordinate_frame = src.coordinate_frame;
  old_row.status = "succeeded";
  old_row.created_at_ns = 1000;
  old_row.document_json = "{}";
  db_.AddReconstruction(old_row);

  auto in = MakeInput(src);
  in.source.reconstruction_id = src.reconstruction_id;
  const auto v2 = ApplyOptimizedTrajectory(in).reconstruction;

  ReconstructionRow new_row;
  new_row.reconstruction_id = ParseUuid(v2.reconstruction_id);
  new_row.scene_id = scene_.scene_id;
  new_row.coordinate_frame = v2.coordinate_frame;
  new_row.status = v2.status;
  new_row.created_at_ns = v2.created_at_ns;
  new_row.document_json = CanonicalJson(v2);
  db_.AddReconstruction(new_row);
  db_.SetReconstructionStatus(old_row.reconstruction_id, "superseded");

  const auto latest = db_.QueryLatestReconstructionByScene(scene_.scene_id);
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(FormatUuid(latest->reconstruction_id), v2.reconstruction_id);
  EXPECT_EQ(latest->status, "succeeded");
}

// ---------------------------------------------------------------------------
// 16. ProvenanceLineage: backend.name="spatial_optimizer", hashes present,
//     acyclic
// ---------------------------------------------------------------------------
TEST(Feedback, ProvenanceLineage) {
  Reconstruction src = MakeSourceRecon();
  src.provenance.input_artifact_hashes = {"source-recon-hash"};
  auto in = MakeInput(src);
  in.source_reconstruction_cas_hash = "source-recon-hash";
  in.optimization_result_cas_hash = "opt-result-hash";
  const auto r = ApplyOptimizedTrajectory(in).reconstruction;

  EXPECT_EQ(r.provenance.backend.name, "spatial_optimizer");
  // The OptimizationResult hash and source Reconstruction hash present.
  const auto& hashes = r.provenance.input_artifact_hashes;
  EXPECT_NE(std::find(hashes.begin(), hashes.end(), "opt-result-hash"),
            hashes.end());
  EXPECT_NE(std::find(hashes.begin(), hashes.end(), "source-recon-hash"),
            hashes.end());
  // backend_specific_json records the optimizer detail (Mapping §8.2: the
  // concrete optimizer lives in backend_specific_json, not backend.name).
  EXPECT_NE(r.provenance.backend_specific_json.find("gtsam"), std::string::npos);
  EXPECT_NE(r.provenance.backend_specific_json.find("option"), std::string::npos);
  // No self-reference: the produced id must not appear in its own provenance.
  for (const auto& h : hashes)
    EXPECT_NE(h, r.reconstruction_id);
}

// ---------------------------------------------------------------------------
// 17. E2E: full chain with numeric assertions
// ---------------------------------------------------------------------------
TEST(Feedback, EndToEnd) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  in.reconstruction_from_trajectory = SE3::Identity();
  const auto v2 = ApplyOptimizedTrajectory(in).reconstruction;

  EXPECT_NE(v2.reconstruction_id, src.reconstruction_id);
  EXPECT_EQ(v2.status, "succeeded");
  // Immutability of the v1 document.
  EXPECT_EQ(src.images[0].pose, MakeSourceRecon().images[0].pose);
  // Points/cameras identical; only updated poses differ.
  EXPECT_EQ(v2.points3D, src.points3D);
  EXPECT_EQ(v2.cameras, src.cameras);
  // Identity alignment -> poses equal the corrected trajectory poses.
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(v2.images[i].pose.translation_xyz, in.optimized_nodes[i].position_xyz);
  }
  EXPECT_TRUE(ValidateOptimizedReconstruction(v2));
  EXPECT_EQ(v2.provenance.backend.name, "spatial_optimizer");
}

// ---------------------------------------------------------------------------
// Additional: empty optimization result fails cleanly; source immutability
// ---------------------------------------------------------------------------
TEST(Feedback, EmptyOptimizationThrows) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  in.optimized_nodes.clear();
  EXPECT_THROW(ApplyOptimizedTrajectory(in), std::invalid_argument);
}

TEST(Feedback, SourceIsNeverMutated) {
  const Reconstruction src = MakeSourceRecon();
  auto in = MakeInput(src);
  in.reconstruction_from_trajectory =
      SE3(Quaternion(0.0, 0.0, 0.7071067811865476, 0.7071067811865476),
          EigenVector3(1.0, 1.0, 1.0));
  const auto orig = src;
  ApplyOptimizedTrajectory(in);
  EXPECT_EQ(src, orig);
}

}  // namespace
}  // namespace spatial::core
