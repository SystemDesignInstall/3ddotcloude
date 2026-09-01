#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "adapters/visual_geometry/fundamental_verifier.h"
#include "adapters/visual_matching/visual_matcher_adapter.h"
#include "core/artifacts/artifact_store.h"
#include "core/errors/project_error.h"
#include "core/loop_closure/correspondence_reconstruction.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/loop_closure/geometric_verifier.h"
#include "core/loop_closure/verification_options.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/feature_extraction.h"
#include "engine/pipeline/loop_closure_verification.h"
#include "schema_check.h"

namespace spatial::engine {
namespace {

using spatial::adapters::visual_geometry::FundamentalGeometricVerifier;
using spatial::adapters::visual_matching::L2NearestMatcher;
using spatial::core::ArtifactStore;
using spatial::core::FeatureCorrespondence;
using spatial::core::FeatureKeypoint;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::GeometricVerificationOptions;
using spatial::core::LoopClosureCandidate;
using spatial::core::LoopClosureVerificationInput;
using spatial::core::MatchingFrameDescriptors;
using spatial::core::MetadataDb;
using spatial::core::Uuid;
using nlohmann::json;

const Uuid kProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

void InsertTestProject(MetadataDb& db) {
  db.InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978", "local",
                   "{}", "{}");
}

void CheckSchema(ArtifactStore& store, const std::string& hash) {
  const auto bytes = store.Get(hash);
  ASSERT_TRUE(bytes.has_value());
  const auto payload = json::parse(std::string(bytes->begin(), bytes->end()));
  std::ifstream in(SPATIAL_LOOPCLOSURE_SCHEMA_JSON);
  ASSERT_TRUE(in.good());
  const auto schema =
      json::parse(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  std::vector<std::string> violations;
  CheckNode(schema, payload, "$", &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();
  ASSERT_EQ(payload["closures"].size(), 1u);
}

// --- Synthetic geometry helpers (deterministic) -----------------------------

// A projective homography producing an epipolar-consistent correspondence set.
// Any point set related by a homography satisfies the epipolar constraint of
// some fundamental matrix, so the 8-point estimator recovers a low-residual F.
Eigen::Matrix3d TestHomography() {
  Eigen::Matrix3d H;
  H << 1.0, 0.0, 20.0,  //
      0.0, 1.0, 5.0,    //
      0.001, -0.0005, 1.0;
  return H;
}

// Applies H to a source 2D point and returns the target 2D point.
std::pair<double, double> HPoint(const Eigen::Matrix3d& H, double x, double y) {
  Eigen::Vector3d p(x, y, 1.0);
  const Eigen::Vector3d q = H * p;
  return {q.x() / q.z(), q.y() / q.z()};
}

// n deterministic source points spread over a region (avoid coincident /
// collinear degeneracy).
std::vector<std::pair<double, double>> SourcePoints(int n) {
  std::vector<std::pair<double, double>> pts;
  const int cols = 6;
  for (int i = 0; i < n; ++i) {
    const int r = i / cols;
    const int c = i % cols;
    const double x = 10.0 + 40.0 * static_cast<double>(c);
    const double y = 10.0 + 40.0 * static_cast<double>(r);
    pts.push_back({x, y});
  }
  return pts;
}

// Deterministic pseudo-random 2D point in [0, w] x [0, h].
std::pair<double, double> RandPoint(std::mt19937& rng, double w, double h) {
  std::uniform_real_distribution<double> ux(0.0, w);
  std::uniform_real_distribution<double> uy(0.0, h);
  return {ux(rng), uy(rng)};
}

MatchingFrameDescriptors MakeFeatures(
    const std::vector<std::pair<double, double>>& keypoints,
    std::int64_t ts, const std::string& frame_id, std::vector<int> seeds) {
  MatchingFrameDescriptors f;
  f.frame_id = frame_id;
  f.timestamp_ns = ts;
  f.descriptor_type = "mock_16";
  for (std::size_t i = 0; i < keypoints.size(); ++i) {
    f.keypoints.push_back(FeatureKeypoint{keypoints[i].first,
                                          keypoints[i].second});
    std::vector<double> row(16, 0.0);
    if (i < seeds.size()) {
      const int s = seeds[i];
      std::uint32_t h = static_cast<std::uint32_t>(s) * 2654435761u;
      for (auto& v : row) {
        h = h * 1664525u + 1013904223u;
        v = static_cast<double>(h >> 8) * (1.0 / 16777216.0);
      }
    }
    f.descriptors.push_back(std::move(row));
  }
  return f;
}

// Builds correspondences linking keypoints[i] (source) <-> kptB[i] (target).
std::vector<FeatureCorrespondence> Pairwise(const std::size_t n) {
  std::vector<FeatureCorrespondence> corr;
  corr.reserve(n);
  for (std::size_t i = 0; i < n; ++i)
    corr.push_back(FeatureCorrespondence{
        static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i), 0.0});
  return corr;
}

LoopClosureVerificationInput MakeInput(
    const MatchingFrameDescriptors& src, const MatchingFrameDescriptors& tgt,
    std::vector<FeatureCorrespondence> corr, bool high_score = true) {
  LoopClosureCandidate cand;
  cand.source_frame_id = src.frame_id;
  cand.target_frame_id = tgt.frame_id;
  cand.feature_match_score = high_score ? 999.0 : 0.0;  // never drives acceptance
  cand.matcher = "visual_l2_nearest";
  LoopClosureVerificationInput in;
  in.candidate = cand;
  in.source = src;
  in.target = tgt;
  in.correspondences = std::move(corr);
  return in;
}

// --- Adapter-level geometric tests (no DB/store) ----------------------------

class FundamentalVerifierTest : public ::testing::Test {
 protected:
  FundamentalGeometricVerifier verifier_;
  GeometricVerificationOptions opts_;
};

TEST_F(FundamentalVerifierTest, ConsistentGeometryIsAccepted) {
  const Eigen::Matrix3d H = TestHomography();
  const int n = 30;
  const auto src_pts = SourcePoints(n);
  std::vector<std::pair<double, double>> tgt_pts;
  for (const auto& p : src_pts) tgt_pts.push_back(HPoint(H, p.first, p.second));
  auto src = MakeFeatures(src_pts, 1000, "src", {});
  auto tgt = MakeFeatures(tgt_pts, 2000, "tgt", {});
  auto in = MakeInput(src, tgt, Pairwise(n));

  const auto res = verifier_.Verify(in, opts_);
  EXPECT_TRUE(res.verified);
  EXPECT_EQ(res.closure.status, "accepted");
  EXPECT_EQ(res.geometric_model, "fundamental");
  EXPECT_EQ(res.correspondence_count, static_cast<std::size_t>(n));
  EXPECT_GE(res.inlier_ratio, opts_.min_inlier_ratio);
  EXPECT_GE(res.inlier_count, static_cast<std::size_t>(opts_.min_inliers));
  EXPECT_LE(res.geometric_residual, opts_.max_residual);
  EXPECT_FALSE(res.has_relative_pose);  // uncalibrated: NO metric pose fabricated
}

TEST_F(FundamentalVerifierTest, OutlierContaminatedConsistentGeometryAccepted) {
  // 40 geometrically consistent + 10 random outliers. RANSAC must recover the
  // true consistent model (inlier ratio ~0.8 -> accepted).
  const Eigen::Matrix3d H = TestHomography();
  const int n_cons = 40, n_out = 10;
  const auto base = SourcePoints(n_cons);
  std::vector<std::pair<double, double>> src_pts = base;
  std::vector<std::pair<double, double>> tgt_pts;
  for (const auto& p : base) tgt_pts.push_back(HPoint(H, p.first, p.second));
  std::mt19937 rng(12345u);
  for (int i = 0; i < n_out; ++i) {
    src_pts.push_back(RandPoint(rng, 300, 300));
    tgt_pts.push_back(RandPoint(rng, 300, 300));
  }
  std::vector<FeatureCorrespondence> corr;
  for (std::size_t i = 0; i < src_pts.size(); ++i)
    corr.push_back(FeatureCorrespondence{static_cast<std::uint32_t>(i),
                                         static_cast<std::uint32_t>(i), 0.0});
  auto src = MakeFeatures(src_pts, 1000, "src", {});
  auto tgt = MakeFeatures(tgt_pts, 2000, "tgt", {});
  auto in = MakeInput(src, tgt, std::move(corr));

  const auto res = verifier_.Verify(in, opts_);
  EXPECT_TRUE(res.verified);
  EXPECT_GE(res.inlier_count, static_cast<std::size_t>(38));
}

TEST_F(FundamentalVerifierTest, VisuallySimilarGeometricallyWrongRejected) {
  // Golden test: candidate has a HIGH feature score but the keypoint geometry
  // is inconsistent (random target points) -> REJECTED. Proves geometry is the
  // deciding layer, not the descriptor similarity score (P3-impl-7b §14/§15).
  const int n = 30;
  const auto src_pts = SourcePoints(n);
  std::mt19937 rng(999u);
  std::vector<std::pair<double, double>> tgt_pts;
  for (int i = 0; i < n; ++i) tgt_pts.push_back(RandPoint(rng, 300, 300));
  auto src = MakeFeatures(src_pts, 1000, "src", {});
  auto tgt = MakeFeatures(tgt_pts, 2000, "tgt", {});
  auto in = MakeInput(src, tgt, Pairwise(n), /*high_score=*/true);

  const auto res = verifier_.Verify(in, opts_);
  EXPECT_FALSE(res.verified);
  EXPECT_EQ(res.closure.status, "rejected");
  EXPECT_LT(res.inlier_ratio, opts_.min_inlier_ratio);
  EXPECT_FALSE(res.inlier_ratio >= opts_.min_inlier_ratio &&
               res.inlier_count >= static_cast<std::size_t>(opts_.min_inliers) &&
               res.geometric_residual <= opts_.max_residual);
  EXPECT_FALSE(res.rejection_reason.empty());
}

TEST_F(FundamentalVerifierTest, InsufficientCorrespondencesRejected) {
  const Eigen::Matrix3d H = TestHomography();
  const int n = 5;  // below min_correspondences (20)
  const auto src_pts = SourcePoints(n);
  std::vector<std::pair<double, double>> tgt_pts;
  for (const auto& p : src_pts) tgt_pts.push_back(HPoint(H, p.first, p.second));
  auto src = MakeFeatures(src_pts, 1000, "src", {});
  auto tgt = MakeFeatures(tgt_pts, 2000, "tgt", {});
  auto in = MakeInput(src, tgt, Pairwise(n));

  const auto res = verifier_.Verify(in, opts_);
  EXPECT_FALSE(res.verified);
  EXPECT_EQ(res.rejection_reason, "insufficient_correspondences");
}

TEST_F(FundamentalVerifierTest, DegenerateCoincidentPointsRejected) {
  // All points identical -> normalization degeneracy -> no stable model.
  const int n = 30;
  std::vector<std::pair<double, double>> src_pts(n, {50.0, 50.0});
  std::vector<std::pair<double, double>> tgt_pts(n, {60.0, 60.0});
  auto src = MakeFeatures(src_pts, 1000, "src", {});
  auto tgt = MakeFeatures(tgt_pts, 2000, "tgt", {});
  auto in = MakeInput(src, tgt, Pairwise(n));

  const auto res = verifier_.Verify(in, opts_);
  EXPECT_FALSE(res.verified);
  EXPECT_EQ(res.closure.status, "rejected");
}

TEST_F(FundamentalVerifierTest, DegenerateTargetCoincidentRejected) {
  // Distinct source points but ALL target points identical: the target-side
  // normalization is degenerate, so no stable fundamental model exists ->
  // REJECTED (P3-impl-7b §16, §12: degenerate geometry fails closed). Note:
  // collinearity alone is not a rejection for an uncalibrated F-verifier (a
  // fundamental matrix can always satisfy collinear correspondences); only a
  // true loss of spatial spread (coincident points) is a stable-estimate
  // degeneracy.
  const int n = 30;
  const auto src_pts = SourcePoints(n);
  std::vector<std::pair<double, double>> tgt_pts(n, {200.0, 200.0});
  auto src = MakeFeatures(src_pts, 1000, "src", {});
  auto tgt = MakeFeatures(tgt_pts, 2000, "tgt", {});
  auto in = MakeInput(src, tgt, Pairwise(n));

  const auto res = verifier_.Verify(in, opts_);
  EXPECT_FALSE(res.verified);
  EXPECT_EQ(res.closure.status, "rejected");
}

TEST_F(FundamentalVerifierTest, BelowMinimumInlierRatioRejected) {
  const Eigen::Matrix3d H = TestHomography();
  const int n_cons = 12, n_out = 23;  // ratio ~0.34 < 0.7
  const auto base = SourcePoints(n_cons);
  std::vector<std::pair<double, double>> src_pts = base;
  std::vector<std::pair<double, double>> tgt_pts;
  for (const auto& p : base) tgt_pts.push_back(HPoint(H, p.first, p.second));
  std::mt19937 rng(555u);
  for (int i = 0; i < n_out; ++i) {
    src_pts.push_back(RandPoint(rng, 300, 300));
    tgt_pts.push_back(RandPoint(rng, 300, 300));
  }
  std::vector<FeatureCorrespondence> corr;
  for (std::size_t i = 0; i < src_pts.size(); ++i)
    corr.push_back(FeatureCorrespondence{static_cast<std::uint32_t>(i),
                                         static_cast<std::uint32_t>(i), 0.0});
  auto src = MakeFeatures(src_pts, 1000, "src", {});
  auto tgt = MakeFeatures(tgt_pts, 2000, "tgt", {});
  auto in = MakeInput(src, tgt, std::move(corr));

  const auto res = verifier_.Verify(in, opts_);
  EXPECT_FALSE(res.verified);
  EXPECT_EQ(res.rejection_reason, "below_minimum_inlier_ratio");
}

TEST_F(FundamentalVerifierTest, DeterministicForIdenticalInputs) {
  const Eigen::Matrix3d H = TestHomography();
  const int n = 30;
  const auto src_pts = SourcePoints(n);
  std::vector<std::pair<double, double>> tgt_pts;
  for (const auto& p : src_pts) tgt_pts.push_back(HPoint(H, p.first, p.second));
  std::mt19937 rng(7u);
  std::vector<std::pair<double, double>> src_o = src_pts;
  std::vector<std::pair<double, double>> tgt_o = tgt_pts;
  for (int i = 0; i < 6; ++i) {  // add a few outliers
    src_o.push_back(RandPoint(rng, 300, 300));
    tgt_o.push_back(RandPoint(rng, 300, 300));
  }
  std::vector<FeatureCorrespondence> corr;
  for (std::size_t i = 0; i < src_o.size(); ++i)
    corr.push_back(FeatureCorrespondence{static_cast<std::uint32_t>(i),
                                         static_cast<std::uint32_t>(i), 0.0});
  auto src = MakeFeatures(src_o, 1000, "src", {});
  auto tgt = MakeFeatures(tgt_o, 2000, "tgt", {});
  auto in = MakeInput(src, tgt, std::move(corr));

  const auto r1 = verifier_.Verify(in, opts_);
  const auto r2 = verifier_.Verify(in, opts_);
  EXPECT_EQ(r1.verified, r2.verified);
  EXPECT_EQ(r1.inlier_count, r2.inlier_count);
  EXPECT_DOUBLE_EQ(r1.inlier_ratio, r2.inlier_ratio);
  EXPECT_DOUBLE_EQ(r1.geometric_residual, r2.geometric_residual);
  EXPECT_EQ(r1.rejection_reason, r2.rejection_reason);
}

TEST_F(FundamentalVerifierTest, MalformedInputThrows) {
  // Empty feature data -> typed verification failure (not a rejection).
  MatchingFrameDescriptors empty;
  empty.frame_id = "src";
  empty.descriptors = {};
  auto in = MakeInput(empty, empty, {});
  EXPECT_THROW(verifier_.Verify(in, opts_),
               spatial::core::ProjectError);
}

TEST_F(FundamentalVerifierTest, OutOfRangeCorrespondenceThrows) {
  auto src = MakeFeatures(SourcePoints(30), 1000, "src", {});
  auto tgt = MakeFeatures(SourcePoints(30), 2000, "tgt", {});
  std::vector<FeatureCorrespondence> corr = Pairwise(30);
  corr[5].target_index = 500;  // out of range
  auto in = MakeInput(src, tgt, std::move(corr));
  EXPECT_THROW(verifier_.Verify(in, opts_), spatial::core::ProjectError);
}

// --- Correspondence reconstruction (core seam) -----------------------------

TEST(ReconstructCorrespondencesTest, MatcherSeamProducesPairs) {
  const L2NearestMatcher matcher;
  // 5 identical-seed descriptors -> 5 mutual correspondences.
  auto src = MakeFeatures(SourcePoints(5), 1000, "src", {1, 2, 3, 4, 5});
  auto tgt = MakeFeatures(SourcePoints(5), 2000, "tgt", {1, 2, 3, 4, 5});
  const auto corr = spatial::core::ReconstructCorrespondences(src, tgt, matcher);
  ASSERT_EQ(corr.size(), 5u);
  for (std::size_t i = 0; i < corr.size(); ++i) {
    EXPECT_EQ(corr[i].source_index, i);
    EXPECT_EQ(corr[i].target_index, i);
  }
}

TEST(ReconstructCorrespondencesTest, CountMismatchThrows) {
  const L2NearestMatcher matcher;
  MatchingFrameDescriptors f;
  f.frame_id = "f";
  f.descriptors = {std::vector<double>(16, 0.0), std::vector<double>(16, 0.0)};
  f.keypoints = {FeatureKeypoint{0, 0}};  // 1 keypoint vs 2 descriptors
  MatchingFrameDescriptors g = f;
  EXPECT_THROW(spatial::core::ReconstructCorrespondences(f, g, matcher),
               spatial::core::ProjectError);
}

// --- Engine orchestration + provenance + schema ----------------------------

class LoopClosureVerificationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_lcv_" + std::to_string(std::time(nullptr)) + "_" +
             std::to_string(rand()));
    std::filesystem::create_directories(root_);
    db_ = MetadataDb::Create(root_ / "project.db");
    InsertTestProject(*db_);
    store_ = std::make_unique<ArtifactStore>(root_ / "artifacts", *db_);
  }

  void TearDown() override {
    store_.reset();
    db_.reset();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Writes a FeatureArtifact from explicitly given keypoints + seeds, and
  // returns the frame input referencing it.
  VerificationFrameInput StoreFeature(const Uuid& frame_id, std::int64_t ts,
                                      std::vector<std::pair<double, double>> pts,
                                      std::vector<int> seeds) {
    WriteFeatureArtifactInput input;
    input.frame_id = frame_id;
    input.detector = "mock";
    input.descriptor_type = "mock_16";
    input.input_content_hash = "image-" + FormatUuid(frame_id);
    for (std::size_t i = 0; i < pts.size(); ++i) {
      input.keypoints.push_back(
          {pts[i].first, pts[i].second, 1.0, 0.0, 0.5});
      input.descriptors.push_back(
          SigRow(i < seeds.size() ? seeds[i] : static_cast<int>(i)));
    }
    const auto res = WriteFeatureArtifactPayload(*store_, input);
    VerificationFrameInput f;
    f.frame_id = frame_id;
    f.timestamp_ns = ts;
    f.feature_artifact_uuid = res.artifact_uuid;
    return f;
  }

  static std::vector<double> SigRow(int seed) {
    std::uint32_t h = static_cast<std::uint32_t>(seed) * 2654435761u;
    std::vector<double> row(16);
    for (auto& v : row) {
      h = h * 1664525u + 1013904223u;
      v = static_cast<double>(h >> 8) * (1.0 / 16777216.0);
    }
    return row;
  }

  LoopClosureCandidate Candidate(const std::string& traj,
                                 const std::string& src, const std::string& tgt,
                                 double score = 999.0) {
    LoopClosureCandidate c;
    c.trajectory_id = traj;
    c.source_frame_id = src;
    c.target_frame_id = tgt;
    c.feature_match_score = score;  // high: must NOT drive acceptance itself
    c.matcher = "visual_l2_nearest";
    c.candidate_id = FormatUuid(GenerateUuid());
    c.created_at_ns = 0;
    return c;
  }

  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> store_;
};

TEST_F(LoopClosureVerificationTest, ValidLoopIsAcceptedAndPersisted) {
  const Eigen::Matrix3d H = TestHomography();
  const Uuid f0 = GenerateUuid(), f1 = GenerateUuid();
  const std::string traj = FormatUuid(GenerateUuid());
  const int n = 30;
  const auto src_pts = SourcePoints(n);
  std::vector<std::pair<double, double>> tgt_pts;
  for (const auto& p : src_pts) tgt_pts.push_back(HPoint(H, p.first, p.second));
  std::vector<int> seeds;
  for (int i = 0; i < n; ++i) seeds.push_back(i);  // identical revisit descriptors

  const auto src_frame = StoreFeature(f0, 10000, src_pts, seeds);
  const auto tgt_frame = StoreFeature(f1, 2000, tgt_pts, seeds);

  const L2NearestMatcher matcher;
  const FundamentalGeometricVerifier verifier;
  const auto cand = Candidate(traj, FormatUuid(f0), FormatUuid(f1));

  const auto res = VerifyLoopClosureGeometry(*store_, *db_, cand, src_frame,
                                             tgt_frame, matcher, verifier, {},
                                             "cfg-hash");
  ASSERT_TRUE(res.artifacts_resolved);
  EXPECT_EQ(res.closure.status, "accepted");
  EXPECT_TRUE(res.closure.candidate_id == cand.candidate_id);
  EXPECT_GT(res.closure.inlier_count, 0);
  EXPECT_FALSE(res.closure.closure_id.empty());

  // Metadata row persisted (both accepted persisted for audit).
  const auto rows = db_->FindLoopClosuresByTrajectory(
      spatial::core::ParseUuid(traj));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].status, "accepted");
  EXPECT_EQ(FormatUuid(rows[0].candidate_id), cand.candidate_id);

  // CAS payload validates against the loop-closure schema + carries provenance
  // (manifest input hashes reference the two FeatureArtifacts).
  CheckSchema(*store_, res.payload_content_hash);
  const auto manifest = store_->ReadManifest(res.payload_artifact_uuid);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->configuration_hash, "cfg-hash");
  ASSERT_EQ(manifest->input_artifact_hashes.size(), 2u);
}

TEST_F(LoopClosureVerificationTest, FalsePositiveCandidateRejectedByGeometry) {
  // Golden test (P3-impl-7b §15): high visual similarity (identical descriptor
  // seeds -> high feature_match_score) but geometrically inconsistent target
  // keypoints (random) -> REJECTED. Candidate != LoopClosure.
  const Uuid f0 = GenerateUuid(), f1 = GenerateUuid();
  const std::string traj = FormatUuid(GenerateUuid());
  const int n = 30;
  const auto src_pts = SourcePoints(n);
  std::mt19937 rng(2024u);
  std::vector<std::pair<double, double>> tgt_pts;
  for (int i = 0; i < n; ++i) tgt_pts.push_back(RandPoint(rng, 300, 300));
  std::vector<int> seeds;
  for (int i = 0; i < n; ++i) seeds.push_back(i);  // identical descriptors

  const auto src_frame = StoreFeature(f0, 10000, src_pts, seeds);
  const auto tgt_frame = StoreFeature(f1, 2000, tgt_pts, seeds);

  const L2NearestMatcher matcher;
  const FundamentalGeometricVerifier verifier;
  const auto cand = Candidate(traj, FormatUuid(f0), FormatUuid(f1), 999.0);

  const auto res = VerifyLoopClosureGeometry(*store_, *db_, cand, src_frame,
                                             tgt_frame, matcher, verifier, {},
                                             "cfg");
  EXPECT_EQ(res.closure.status, "rejected");
  EXPECT_FALSE(res.geo.verified);

  // Both accepted AND rejected closures are persisted for audit (D-LC-05).
  const auto rows = db_->FindLoopClosuresByTrajectory(
      spatial::core::ParseUuid(traj));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].status, "rejected");
}

TEST_F(LoopClosureVerificationTest, MissingArtifactFailsClosedWithTypedError) {
  const Uuid f0 = GenerateUuid(), f1 = GenerateUuid();
  const std::string traj = FormatUuid(GenerateUuid());
  VerificationFrameInput good =
      StoreFeature(f0, 10000, SourcePoints(30), {});
  VerificationFrameInput missing = {f1, 2000, GenerateUuid()};  // not stored

  const L2NearestMatcher matcher;
  const FundamentalGeometricVerifier verifier;
  const auto cand = Candidate(traj, FormatUuid(f0), FormatUuid(f1));

  EXPECT_THROW(VerifyLoopClosureGeometry(*store_, *db_, cand, good, missing,
                                         matcher, verifier, {}, "cfg"),
               spatial::core::ProjectError);
}

}  // namespace
}  // namespace spatial::engine
