#include "engine/pipeline/quality/quality_report.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/project_error.h"
#include "core/utils/fs.h"
#include "core/utils/sha256.h"
#include "engine_build_info.h"

namespace spatial::engine {
namespace {

using spatial::core::ErrorCode;
using spatial::core::ProjectError;
using spatial::core::Sha256Hex;
using spatial::core::fs::Iso8601UtcNow;
using nlohmann::json;

// splitmix64: a small deterministic PRNG seeded from the SHA-256 digest of
// the run identity. RFC-0005 §2 forbids wall-clock randomness in the metrics,
// so every metric value is a pure function of the pipeline hash + stage +
// effective configuration + consumed input refs.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

 private:
  std::uint64_t state_;
};

// Uniform double in [0, 1).
double Unit(SplitMix64& rng) {
  return static_cast<double>(rng.Next() >> 11) *
         (1.0 / 9007199254740992.0);
}

double Clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

// Canonical run seed: the same run identity always yields the same digest.
// Input refs are sorted so caller order does not matter (ADR-020 convention).
std::string RunSeed(const std::string& pipeline_hash,
                    const std::string& stage_id,
                    const std::string& config_json,
                    const std::vector<ArtifactRef>& input_refs) {
  std::vector<ArtifactRef> sorted = input_refs;
  std::sort(sorted.begin(), sorted.end());
  std::string content = pipeline_hash;
  content += "|" + stage_id;
  content += "|" + config_json;
  for (const auto& ref : sorted) {
    content += "|" + ref;
  }
  return Sha256Hex(content);
}

std::uint64_t SeedWord(const std::string& digest) {
  std::uint64_t out = 0;
  for (std::size_t i = 0; i < digest.size() && i < 16; ++i) {
    out = (out << 4) | (digest[i] <= '9' ? digest[i] - '0'
                                         : digest[i] - 'a' + 10);
  }
  return out;
}

// Effective thresholds: config.quality.thresholds.{pass,warn} over the
// defaults 80/50. Any non-object intermediate is ignored (nlohmann::json
// throws type_error.306 on value() against a scalar), so unknown config
// shapes fall back to the defaults. `pass` is clamped to be at least `warn`
// so the verdict classification stays well-defined.
QualityThresholds ParseThresholds(const json& config_json) {
  QualityThresholds thresholds;
  if (!config_json.is_object()) {
    return thresholds;
  }
  const auto config = config_json.value("config", json::object());
  if (!config.is_object()) {
    return thresholds;
  }
  const auto quality = config.value("quality", json::object());
  if (!quality.is_object()) {
    return thresholds;
  }
  const auto t = quality.value("thresholds", json::object());
  if (t.is_object()) {
    if (t.contains("pass") && t["pass"].is_number()) {
      thresholds.pass = t["pass"].get<double>();
    }
    if (t.contains("warn") && t["warn"].is_number()) {
      thresholds.warn = t["warn"].get<double>();
    }
  }
  thresholds.pass = std::max(thresholds.pass, thresholds.warn);
  return thresholds;
}

// Aggregates the known metric groups into a 0-100 score (RFC-0005 §5.4):
// each group normalized against a documented reference threshold, then
// weighted. Post-M0 groups are governed by ADR-030.
double AggregateScore(const ReprojectionMetrics& reprojection,
                      const CoverageMetrics& coverage,
                      const GeometryMetrics& geometry) {
  constexpr double kRmseReferencePx = 1.5;
  const double reprojection_sub =
      100.0 * Clamp01(1.0 - reprojection.rmse_px / kRmseReferencePx);
  const double coverage_sub =
      100.0 * Clamp01(coverage.completeness_pct / 100.0);
  const double geometry_sub = 100.0 * Clamp01(geometry.mean_confidence);
  return 0.5 * reprojection_sub + 0.3 * coverage_sub + 0.2 * geometry_sub;
}

QualityVerdict Classify(double score, const QualityThresholds& thresholds) {
  if (score >= thresholds.pass) {
    return QualityVerdict::kPass;
  }
  if (score >= thresholds.warn) {
    return QualityVerdict::kWarn;
  }
  return QualityVerdict::kFail;
}

}  // namespace

const char* QualityVerdictName(QualityVerdict verdict) noexcept {
  switch (verdict) {
    case QualityVerdict::kPass:
      return "pass";
    case QualityVerdict::kWarn:
      return "warn";
    case QualityVerdict::kFail:
      return "fail";
  }
  return "fail";
}

QualityReport EvaluateQuality(const std::string& pipeline_hash,
                              const std::string& stage_id,
                              const std::string& config_json,
                              const std::vector<ArtifactRef>& input_refs) {
  const std::string digest = RunSeed(pipeline_hash, stage_id, config_json,
                                     input_refs);
  SplitMix64 rng(SeedWord(digest));

  // Plausible synthesized metrics for the mock stage (P1.5). Real metric
  // values arrive with the Scene-level quality engine (ADR-030, post-M0).
  QualityReport report;
  report.pipeline_hash = pipeline_hash;
  report.stage_id = stage_id;
  report.engine_name = "spatial-platform";
  report.engine_version = kEngineVersion;
  report.engine_git_commit = kEngineGitCommit;
  report.thresholds = ParseThresholds(json::parse(config_json));

  report.reprojection.rmse_px = 0.2 + 1.3 * Unit(rng);
  report.reprojection.mean_error_px = 0.7 * report.reprojection.rmse_px;
  report.coverage.completeness_pct = 80.0 + 20.0 * Unit(rng);
  report.coverage.baseline_ratio = 0.2 + 0.3 * Unit(rng);
  report.geometry.point_count = 30000.0 + 30000.0 * Unit(rng);
  report.geometry.mean_confidence = 0.5 + 0.5 * Unit(rng);

  report.quality_score = AggregateScore(report.reprojection,
                                        report.coverage, report.geometry);
  report.verdict = Classify(report.quality_score, report.thresholds);
  report.generated_at = Iso8601UtcNow();
  return report;
}

std::string QualityReportToJson(const QualityReport& report) {
  const json j = {
      {"pipeline_hash", report.pipeline_hash},
      {"stage_id", report.stage_id},
      {"quality_engine",
       {{"name", report.engine_name},
        {"version", report.engine_version},
        {"git_commit", report.engine_git_commit}}},
      {"thresholds",
       {{"quality_score",
         {{"pass", report.thresholds.pass}, {"warn", report.thresholds.warn}}}}},
      {"metrics",
       {{"reprojection",
         {{"rmse_px", report.reprojection.rmse_px},
          {"mean_error_px", report.reprojection.mean_error_px}}},
        {"coverage",
         {{"completeness_pct", report.coverage.completeness_pct},
          {"baseline_ratio", report.coverage.baseline_ratio}}},
        {"geometry",
         {{"point_count", report.geometry.point_count},
          {"mean_confidence", report.geometry.mean_confidence}}}}},
      {"quality_score", report.quality_score},
      {"verdict", QualityVerdictName(report.verdict)},
      {"generated_at", report.generated_at},
  };
  return j.dump(2);
}

QualityReport QualityReportFromJson(const std::string& report_json) {
  json parsed;
  try {
    parsed = json::parse(report_json);
  } catch (const json::parse_error&) {
    throw ProjectError(ErrorCode::kValidationDomain,
                       "quality report is not valid JSON");
  }
  const auto require = [&parsed](const char* key) -> const json& {
    if (!parsed.contains(key)) {
      throw ProjectError(ErrorCode::kValidationDomain,
                         std::string("quality report is missing '") + key +
                             "'");
    }
    return parsed[key];
  };

  QualityReport report;
  report.pipeline_hash = require("pipeline_hash").get<std::string>();
  report.stage_id = require("stage_id").get<std::string>();
  const auto& engine = require("quality_engine");
  if (engine.is_object()) {
    report.engine_name = engine.value("name", "");
    report.engine_version = engine.value("version", "");
    report.engine_git_commit = engine.value("git_commit", "");
  }
  const auto& thresholds = require("thresholds");
  const auto quality_score_thresholds =
      thresholds.is_object()
          ? thresholds.value("quality_score", json::object())
          : json::object();
  report.thresholds.pass =
      quality_score_thresholds.is_object()
          ? quality_score_thresholds.value("pass", report.thresholds.pass)
          : report.thresholds.pass;
  report.thresholds.warn =
      quality_score_thresholds.is_object()
          ? quality_score_thresholds.value("warn", report.thresholds.warn)
          : report.thresholds.warn;

  const auto& metrics = require("metrics");
  const auto reprojection =
      metrics.is_object() ? metrics.value("reprojection", json::object())
                          : json::object();
  if (reprojection.is_object()) {
    report.reprojection.rmse_px = reprojection.value("rmse_px", 0.0);
    report.reprojection.mean_error_px =
        reprojection.value("mean_error_px", 0.0);
  }
  const auto coverage =
      metrics.is_object() ? metrics.value("coverage", json::object())
                          : json::object();
  if (coverage.is_object()) {
    report.coverage.completeness_pct =
        coverage.value("completeness_pct", 0.0);
    report.coverage.baseline_ratio = coverage.value("baseline_ratio", 0.0);
  }
  const auto geometry =
      metrics.is_object() ? metrics.value("geometry", json::object())
                          : json::object();
  if (geometry.is_object()) {
    report.geometry.point_count = geometry.value("point_count", 0.0);
    report.geometry.mean_confidence =
        geometry.value("mean_confidence", 0.0);
  }

  report.quality_score = require("quality_score").get<double>();
  const std::string verdict = require("verdict").get<std::string>();
  if (verdict == "pass") {
    report.verdict = QualityVerdict::kPass;
  } else if (verdict == "warn") {
    report.verdict = QualityVerdict::kWarn;
  } else {
    report.verdict = QualityVerdict::kFail;
  }
  report.generated_at = parsed.value("generated_at", "");
  return report;
}

}  // namespace spatial::engine
