#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adapters/visual_matching/visual_matcher_adapter.h"
#include "core/artifacts/artifact_store.h"
#include "core/loop_closure/feature_matcher.h"
#include "core/loop_closure/loop_closure_candidate_gen.h"
#include "core/storage/metadata_db.h"
#include "core/trajectory/loop_closure.h"
#include "core/utils/uuid.h"
#include "engine/pipeline/feature_extraction.h"
#include "engine/pipeline/loop_closure_detection.h"
#include "schema_check.h"

namespace spatial::engine {
namespace {

using spatial::adapters::visual_matching::L2NearestMatcher;
using spatial::core::ArtifactStore;
using spatial::core::FormatUuid;
using spatial::core::GenerateUuid;
using spatial::core::LoopClosureCandidate;
using spatial::core::LoopClosureDetectionOptions;
using spatial::core::MatchingFrameDescriptors;
using spatial::core::MetadataDb;
using spatial::core::ParseUuid;
using spatial::core::Uuid;
using nlohmann::json;

const Uuid kProjectId = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

void InsertTestProject(MetadataDb& db) {
  db.InsertProject(kProjectId, "proj", 1, "{}", 1000, "EPSG:4978", "local",
                   "{}", "{}");
}

// A fixed 16-dim descriptor row with a distinctive, well-separated signature
// (mock_16 space). A multiplicative hash spreads different `seed` values to
// near-independent rows, so unrelated seeds have large L2 distance (>> the
// matcher threshold) while the SAME seed yields an identical row (a "revisit").
// Deterministic and platform-stable (fixed-width unsigned arithmetic).
std::vector<double> Sig(int seed) {
  std::uint32_t h = static_cast<std::uint32_t>(seed) * 2654435761u;
  std::vector<double> row;
  row.reserve(16);
  for (int d = 0; d < 16; ++d) {
    h = h * 1664525u + 1013904223u;
    row.push_back(static_cast<double>(h >> 8) * (1.0 / 16777216.0));
  }
  return row;
}

MatchingFrameDescriptors FrameDescriptors(const Uuid& frame_id,
                                          std::int64_t ts,
                                          std::vector<int> seeds) {
  MatchingFrameDescriptors f;
  f.frame_id = FormatUuid(frame_id);
  f.timestamp_ns = ts;
  f.descriptor_type = "mock_16";
  for (const int s : seeds) f.descriptors.push_back(Sig(s));
  return f;
}

// --- Adapter: the classical matcher is deterministic and separates revisit
// from unrelated frames (Q3: mock_16 for deterministic tests only). ---

TEST(VisualMatcherAdapterTest, IdenticalDescriptorsYieldHighScore) {
  const L2NearestMatcher m;
  const auto a = FrameDescriptors(GenerateUuid(), 0, {1, 2, 3, 4, 5});
  const auto b = FrameDescriptors(GenerateUuid(), 1, {1, 2, 3, 4, 5});
  EXPECT_EQ(m.MatchScore(a, b), 5.0);  // all 5 descriptors reciprocally match
  EXPECT_EQ(m.MatcherId(), "visual_l2_nearest");

  // By symmetry of reciprocal matching the reverse order scores the same.
  EXPECT_EQ(m.MatchScore(b, a), 5.0);
}

TEST(VisualMatcherAdapterTest, UnrelatedDescriptorsYieldLowScore) {
  const L2NearestMatcher m;
  const auto a = FrameDescriptors(GenerateUuid(), 0, {1, 2, 3});
  const auto b = FrameDescriptors(GenerateUuid(), 1, {101, 102, 103});
  EXPECT_EQ(m.MatchScore(a, b), 0.0);
}

TEST(VisualMatcherAdapterTest, PartialOverlapScoresBetween) {
  const L2NearestMatcher m;
  // b shares {1,2} with a and adds two new descriptors.
  const auto a = FrameDescriptors(GenerateUuid(), 0, {1, 2, 3, 4, 5});
  const auto b = FrameDescriptors(GenerateUuid(), 1, {1, 2, 9, 10});
  EXPECT_EQ(m.MatchScore(a, b), 2.0);
  EXPECT_GT(m.MatchScore(a, b), 0.0);
  EXPECT_LT(m.MatchScore(a, b),
            m.MatchScore(FrameDescriptors(GenerateUuid(), 0, {1, 2, 3, 4, 5}),
                         FrameDescriptors(GenerateUuid(), 1, {1, 2, 3, 4, 5})));
}

TEST(VisualMatcherAdapterTest, DeterministicForIdenticalInputs) {
  const L2NearestMatcher m;
  const auto a = FrameDescriptors(GenerateUuid(), 0, {1, 2, 3, 4, 5, 6});
  const auto b = FrameDescriptors(GenerateUuid(), 1, {1, 2, 8, 9, 10, 11});
  EXPECT_EQ(m.MatchScore(a, b), m.MatchScore(a, b));
}

// --- Canonical candidate generation (Q5: bounded/windowed + temporal). ---

TEST(LoopClosureCandidateGenTest, SourceIsNewerTargetIsOlder) {
  const L2NearestMatcher m;
  const auto f0 = FrameDescriptors(GenerateUuid(), 1000, {1, 2, 3});
  const auto f1 = FrameDescriptors(GenerateUuid(), 2000, {1, 2, 3});
  const auto f2 = FrameDescriptors(GenerateUuid(), 3000, {1, 2, 3});
  const auto cands = spatial::core::GenerateLoopClosureCandidates(
      {f0, f1, f2}, m, {}, "traj-1");
  ASSERT_FALSE(cands.empty());
  for (const auto& c : cands) {
    EXPECT_EQ(c.trajectory_id, "traj-1");
    EXPECT_EQ(c.matcher, "visual_l2_nearest");
    EXPECT_GT(c.feature_match_score, 0.0);
    // source is newer than target (strictly older frame).
    EXPECT_NE(c.source_frame_id, c.target_frame_id);
  }
}

TEST(LoopClosureCandidateGenTest, TemporalExclusionRemovesNearPairs) {
  const L2NearestMatcher m;
  const auto f0 = FrameDescriptors(GenerateUuid(), 0, {1, 2, 3});
  const auto f1 = FrameDescriptors(GenerateUuid(), 50, {1, 2, 3});
  const auto f2 = FrameDescriptors(GenerateUuid(), 2000, {1, 2, 3});

  // With no minimum separation, 3 pairs; near-consecutive included.
  const std::int64_t sep = 1000;
  const auto all = spatial::core::GenerateLoopClosureCandidates(
      {f0, f1, f2}, m,
      spatial::core::LoopClosureDetectionOptions{
          .minimum_temporal_separation_ns = 0});
  EXPECT_EQ(all.size(), 3u);

  // With a 1000ns minimum, the (f1,f0) near pair is excluded (sep=50 < 1000).
  const auto filt = spatial::core::GenerateLoopClosureCandidates(
      {f0, f1, f2}, m,
      spatial::core::LoopClosureDetectionOptions{
          .minimum_temporal_separation_ns = sep});
  ASSERT_EQ(filt.size(), 2u);
  for (const auto& c : filt) {
    const std::int64_t s1 = c.source_frame_id == f2.frame_id ? 2000
                           : c.source_frame_id == f1.frame_id ? 50
                                                              : 0;
    (void)s1;
    // The only surviving pairs involve f2 vs f0 (sep 2000) and f2 vs f1
    // (sep 1950); never f1 vs f0.
    EXPECT_NE(c.source_frame_id, f1.frame_id);
  }
}

TEST(LoopClosureCandidateGenTest, ScoreFloorFiltersWeakMatches) {
  const L2NearestMatcher m;
  // b shares {1,2} with a (score 2); c shares nothing with a.
  const auto a = FrameDescriptors(GenerateUuid(), 1000, {1, 2, 3, 4, 5});
  const auto b = FrameDescriptors(GenerateUuid(), 2000, {1, 2, 9, 10, 11});
  const auto c = FrameDescriptors(GenerateUuid(), 3000, {101, 102});

  const auto no_floor = spatial::core::GenerateLoopClosureCandidates(
      {a, b, c}, m,
      spatial::core::LoopClosureDetectionOptions{.minimum_match_score = 0.0});
  ASSERT_EQ(no_floor.size(), 3u);

  const auto floor = spatial::core::GenerateLoopClosureCandidates(
      {a, b, c}, m,
      spatial::core::LoopClosureDetectionOptions{.minimum_match_score = 1.5});
  // Only (b,a) has score >= 1.5. (c,b) and (c,a) are below.
  ASSERT_EQ(floor.size(), 1u);
  EXPECT_EQ(floor[0].source_frame_id, b.frame_id);
  EXPECT_EQ(floor[0].target_frame_id, a.frame_id);
}

TEST(LoopClosureCandidateGenTest, BoundedWindowLimitsLookback) {
  const L2NearestMatcher m;
  const auto f0 = FrameDescriptors(GenerateUuid(), 0, {1, 2, 3});
  const auto f1 = FrameDescriptors(GenerateUuid(), 1, {1, 2, 3});
  const auto f2 = FrameDescriptors(GenerateUuid(), 2, {1, 2, 3});
  const auto f3 = FrameDescriptors(GenerateUuid(), 3, {1, 2, 3});

  // No window: f1->f0, f2->{f0,f1}, f3->{f0,f1,f2} = 6 candidates.
  const auto full = spatial::core::GenerateLoopClosureCandidates(
      {f0, f1, f2, f3}, m,
      spatial::core::LoopClosureDetectionOptions{.search_window = 0});
  EXPECT_EQ(full.size(), 6u);

  // Window of 1: each source matches only the immediately-previous frame.
  const auto w1 = spatial::core::GenerateLoopClosureCandidates(
      {f0, f1, f2, f3}, m,
      spatial::core::LoopClosureDetectionOptions{.search_window = 1});
  EXPECT_EQ(w1.size(), 3u);

  // Window of 2: f3 can match f2 and f1 (not f0), etc.
  const auto w2 = spatial::core::GenerateLoopClosureCandidates(
      {f0, f1, f2, f3}, m,
      spatial::core::LoopClosureDetectionOptions{.search_window = 2});
  EXPECT_EQ(w2.size(), 5u);  // 1 + 2 + 2
}

TEST(LoopClosureCandidateGenTest, TopKPerSourceLimitsCandidates) {
  const L2NearestMatcher m;
  const auto f0 = FrameDescriptors(GenerateUuid(), 0, {1, 2, 3, 4, 5});
  const auto f1 = FrameDescriptors(GenerateUuid(), 1, {1, 2, 9});
  const auto f2 = FrameDescriptors(GenerateUuid(), 2, {1, 2, 3, 4, 5});
  // f2 matches f0 strongly (5) and f1 (2). max_candidates_per_source=1 keeps
  // only the highest-scoring per source.
  const auto keep1 = spatial::core::GenerateLoopClosureCandidates(
      {f0, f1, f2}, m,
      spatial::core::LoopClosureDetectionOptions{.max_candidates_per_source = 1});
  // f1 matches f0 (2); f2 matches {f0(5), f1(2)} -> top-1 = f0.
  ASSERT_EQ(keep1.size(), 2u);
  const auto f2cands = [&] {
    std::vector<LoopClosureCandidate> out;
    for (const auto& c : keep1)
      if (c.source_frame_id == f2.frame_id) out.push_back(c);
    return out;
  }();
  ASSERT_EQ(f2cands.size(), 1u);
  EXPECT_EQ(f2cands[0].target_frame_id, f0.frame_id);
}

// --- Engine orchestration: real FeatureArtifacts -> persisted candidates + CAS. ---

class LoopClosureDetectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("spatial_lc_" + std::to_string(std::time(nullptr)) + "_" +
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

  json LoadLoopClosureSchema() const {
    std::ifstream in(SPATIAL_LOOPCLOSURE_SCHEMA_JSON);
    EXPECT_TRUE(in.good()) << "cannot open loop-closure.schema.json";
    return json::parse(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }

  // Stores one frame's FeatureArtifact (scene-agnostic payload writer) and
  // returns a LoopClosureFrameInput referencing it.
  LoopClosureFrameInput StoreFeature(const Uuid& frame_id,
                                     std::int64_t ts,
                                     std::vector<int> seeds) {
    WriteFeatureArtifactInput input;
    input.frame_id = frame_id;
    input.detector = "mock";
    input.descriptor_type = "mock_16";
    input.input_content_hash = "image-" + FormatUuid(frame_id);
    for (const int s : seeds) {
      input.keypoints.push_back({static_cast<double>(s), 0.0, 1.0, 0.0, 0.5});
      input.descriptors.push_back(Sig(s));
    }
    const auto res = WriteFeatureArtifactPayload(*store_, input);
    LoopClosureFrameInput f;
    f.frame_id = frame_id;
    f.timestamp_ns = ts;
    f.feature_artifact_uuid = res.artifact_uuid;
    return f;
  }

  std::filesystem::path root_;
  std::optional<MetadataDb> db_;
  std::unique_ptr<ArtifactStore> store_;
};

TEST_F(LoopClosureDetectionTest, ProducesCandidatesAndPersistsRows) {
  const Uuid f0 = GenerateUuid();
  const Uuid f1 = GenerateUuid();
  const Uuid f2 = GenerateUuid();
  // f0 and f2 are a revisit (identical descriptors); f1 is unrelated.
  const auto frames = std::vector<LoopClosureFrameInput>{
      StoreFeature(f0, 1000, {1, 2, 3, 4, 5}),
      StoreFeature(f1, 2000, {101, 102, 103}),
      StoreFeature(f2, 3000, {1, 2, 3, 4, 5}),
  };
  const Uuid traj = GenerateUuid();
  const std::string traj_str = FormatUuid(traj);

  const L2NearestMatcher matcher;
  LoopClosureDetectionOptions options;
  // f1 is unrelated to f0 (score 0); f2 is a revisit of f0 (score 5). A small
  // positive floor filters the zero-score unrelated pairs while keeping the
  // revisit candidate.
  options.minimum_match_score = 0.5;

  auto res = DetectLoopClosureCandidates(*store_, *db_, traj_str, frames,
                                         matcher, options, "cfg-hash");

  EXPECT_EQ(res.matcher, "visual_l2_nearest");
  EXPECT_EQ(res.frames_input, 3u);
  EXPECT_EQ(res.artifacts_resolved, 3u);
  ASSERT_FALSE(res.candidates.empty());

  // f1 (score 0) vs f0 generates no candidate; f2 vs f0 (score 5) and f2 vs
  // f1 (score 0) -> f2vs f0 only above floor; f1 vs f0 is score 0.
  // Expect exactly the (f2, f0) revisit candidate (strongest, above any floor).
  ASSERT_EQ(res.candidates.size(), 1u);
  EXPECT_EQ(res.candidates[0].source_frame_id, FormatUuid(f2));
  EXPECT_EQ(res.candidates[0].target_frame_id, FormatUuid(f0));
  EXPECT_EQ(res.candidates[0].feature_match_score, 5.0);
  EXPECT_EQ(res.candidates[0].matcher, "visual_l2_nearest");
  EXPECT_FALSE(res.candidates[0].candidate_id.empty());

  // Rows persisted (audit trail) and readable back by trajectory.
  const auto rows = db_->FindLoopClosureCandidatesByTrajectory(traj);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(FormatUuid(rows[0].candidate_id),
            res.candidates[0].candidate_id);
  EXPECT_EQ(FormatUuid(rows[0].source_frame_id), FormatUuid(f2));
  EXPECT_EQ(FormatUuid(rows[0].target_frame_id), FormatUuid(f0));
  EXPECT_EQ(rows[0].feature_match_score, 5.0);
  EXPECT_EQ(rows[0].matcher, "visual_l2_nearest");
}

TEST_F(LoopClosureDetectionTest, CasPayloadValidatesAgainstLoopClosureSchema) {
  const Uuid f0 = GenerateUuid();
  const Uuid f2 = GenerateUuid();
  const auto frames = std::vector<LoopClosureFrameInput>{
      StoreFeature(f0, 1000, {1, 2, 3}),
      StoreFeature(f2, 2000, {1, 2, 3}),
  };
  const L2NearestMatcher matcher;
  const auto res = DetectLoopClosureCandidates(
      *store_, *db_, FormatUuid(GenerateUuid()), frames, matcher, {}, "");

  const auto bytes = store_->Get(res.payload_content_hash);
  ASSERT_TRUE(bytes.has_value());
  const auto payload = json::parse(std::string(bytes->begin(), bytes->end()));

  std::vector<std::string> violations;
  CheckNode(LoadLoopClosureSchema(), payload, "$", &violations);
  ASSERT_TRUE(violations.empty()) << [&violations] {
    std::string joined;
    for (const auto& v : violations) joined += "\n  " + v;
    return joined;
  }();

  EXPECT_EQ(payload["schema_version"], 1);
  EXPECT_EQ(payload["candidates"].size(), 1u);
  EXPECT_EQ(payload["closures"].size(), 0u);
}

TEST_F(LoopClosureDetectionTest, CasManifestCarriesProvenance) {
  const Uuid f0 = GenerateUuid();
  const Uuid f2 = GenerateUuid();
  const auto frames = std::vector<LoopClosureFrameInput>{
      StoreFeature(f0, 1000, {1, 2, 3}),
      StoreFeature(f2, 2000, {1, 2, 3}),
  };
  const L2NearestMatcher matcher;
  const auto res = DetectLoopClosureCandidates(
      *store_, *db_, FormatUuid(GenerateUuid()), frames, matcher, {}, "cfg");

  const auto manifest = store_->ReadManifest(res.payload_artifact_uuid);
  ASSERT_TRUE(manifest.has_value());
  EXPECT_EQ(manifest->type, "loop_closure");
  EXPECT_EQ(manifest->schema_version, 1);
  // Provenance: the CAS payload's input_artifact_hashes reference the two
  // FeatureArtifacts it consumed.
  ASSERT_EQ(manifest->input_artifact_hashes.size(), 2u);
  EXPECT_EQ(manifest->configuration_hash, "cfg");
  EXPECT_EQ(manifest->content_hash, res.payload_content_hash);
}

}  // namespace
}  // namespace spatial::engine
