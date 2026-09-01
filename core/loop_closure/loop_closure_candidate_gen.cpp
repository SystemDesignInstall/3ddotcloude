#include "core/loop_closure/loop_closure_candidate_gen.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/uuid.h"

namespace spatial::core {

using nlohmann::json;

namespace {

// Parses a raw FeatureArtifact payload (feature.schema.json) into a
// MatchingFrameDescriptors and enforces the producer guarantee
// count == keypoints.length == descriptors.length (RFC-0007 §2, also enforced
// on write in feature_extraction.cpp).
MatchingFrameDescriptors ParseFeaturePayload(
    const std::vector<std::uint8_t>& bytes, const std::string& frame_id,
    std::int64_t timestamp_ns) {
  json payload;
  try {
    payload = json::parse(std::string(bytes.begin(), bytes.end()));
  } catch (const json::exception&) {
    throw ProjectError(ErrorCode::kValidationDomain,
                       "loop closure: malformed feature payload JSON");
  }

  const auto count = payload.value("count", std::int64_t{0});
  const auto keypoints = payload.value("keypoints", json::array());
  const auto descriptors = payload.value("descriptors", json::array());
  if (count != static_cast<std::int64_t>(keypoints.size()) ||
      count != static_cast<std::int64_t>(descriptors.size())) {
    throw ProjectError(ErrorCode::kValidationDomain,
                       "loop closure: feature payload violates producer "
                       "guarantee (count != keypoints != descriptors)");
  }

  MatchingFrameDescriptors out;
  out.frame_id = frame_id;
  out.timestamp_ns = timestamp_ns;
  out.descriptor_type = payload.value("descriptor_type", std::string{});
  // P3-impl-7b: capture image-space keypoint coordinates too (additive field),
  // so geometric verification can reconstruct (x,y) correspondences. The
  // count == keypoints == descriptors guarantee is already enforced above.
  out.keypoints.reserve(keypoints.size());
  for (const auto& kp : keypoints) {
    FeatureKeypoint p;
    p.x = kp.value("x", 0.0);
    p.y = kp.value("y", 0.0);
    out.keypoints.push_back(p);
  }
  out.descriptors.reserve(descriptors.size());
  for (const auto& row : descriptors) {
    std::vector<double> d;
    d.reserve(row.size());
    for (const auto& v : row) d.push_back(v.get<double>());
    out.descriptors.push_back(std::move(d));
  }
  return out;
}

}  // namespace

std::optional<MatchingFrameDescriptors> LoadFeatureDescriptors(
    ArtifactStore& store, const Uuid& artifact_uuid,
    const std::string& frame_id, std::int64_t timestamp_ns) {
  const auto manifest = store.ReadManifest(artifact_uuid);
  if (!manifest) return std::nullopt;
  if (manifest->type != "feature") return std::nullopt;

  const auto bytes = store.Get(manifest->content_hash);
  if (!bytes) return std::nullopt;
  return ParseFeaturePayload(*bytes, frame_id, timestamp_ns);
}

std::vector<LoopClosureCandidate> GenerateLoopClosureCandidates(
    const std::vector<MatchingFrameDescriptors>& frames,
    const LoopClosureFeatureMatcher& matcher,
    const LoopClosureDetectionOptions& options,
    const std::string& trajectory_id) {
  std::vector<LoopClosureCandidate> candidates;
  for (std::size_t i = 1; i < frames.size(); ++i) {
    const std::size_t lower =
        (options.search_window == 0 || options.search_window >= i)
            ? 0
            : i - options.search_window;
    // Source i matches only OLDER frames j (target), never future frames.
    // Candidates are ranked by score; keep top-k above the floor per source.
    std::vector<LoopClosureCandidate> per_source;
    for (std::size_t j = lower; j < i; ++j) {
      const std::int64_t sep = frames[i].timestamp_ns - frames[j].timestamp_ns;
      if (sep < options.minimum_temporal_separation_ns) continue;

      LoopClosureCandidate c;
      c.trajectory_id = trajectory_id;
      c.source_frame_id = frames[i].frame_id;  // newer
      c.target_frame_id = frames[j].frame_id;  // older / revisited
      c.feature_match_score =
          matcher.MatchScore(frames[i], frames[j]);
      c.matcher = matcher.MatcherId();
      if (c.feature_match_score < options.minimum_match_score) continue;
      per_source.push_back(std::move(c));
    }

    std::sort(per_source.begin(), per_source.end(),
              [](const LoopClosureCandidate& a,
                 const LoopClosureCandidate& b) {
                return a.feature_match_score > b.feature_match_score;
              });
    const std::size_t keep =
        (options.max_candidates_per_source == 0)
            ? per_source.size()
            : std::min(per_source.size(), options.max_candidates_per_source);
    for (std::size_t k = 0; k < keep; ++k) {
      per_source[k].candidate_id = FormatUuid(GenerateUuid());
      per_source[k].created_at_ns = fs::TimestampNsNow();
      candidates.push_back(std::move(per_source[k]));
    }
  }
  return candidates;
}

}  // namespace spatial::core
