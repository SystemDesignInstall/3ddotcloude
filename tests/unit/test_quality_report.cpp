// Quality engine tests (RFC-0005 P1.5). EvaluateQuality is a pure function of
// the run: identical inputs produce identical metrics, score, and verdict; the
// report round-trips through QualityReportToJson/FromJson; and the JSON schema
// stays open for additive metric groups (Schema Evolution Policy, RFC-0005
// §5.5). Real metric values arrive with the Scene-level engine (ADR-030).

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/pipeline/quality/quality_report.h"

namespace spatial::engine {
namespace {


TEST(QualityReportTest, DeterministicEvaluation) {
  const auto a = EvaluateQuality("hash-a", "validate", R"({"config":{}})",
                                 {"ref-1"});
  const auto b = EvaluateQuality("hash-a", "validate", R"({"config":{}})",
                                 {"ref-1"});
  EXPECT_EQ(a.reprojection.rmse_px, b.reprojection.rmse_px);
  EXPECT_EQ(a.reprojection.mean_error_px, b.reprojection.mean_error_px);
  EXPECT_EQ(a.coverage.completeness_pct, b.coverage.completeness_pct);
  EXPECT_EQ(a.coverage.baseline_ratio, b.coverage.baseline_ratio);
  EXPECT_EQ(a.geometry.point_count, b.geometry.point_count);
  EXPECT_EQ(a.geometry.mean_confidence, b.geometry.mean_confidence);
  EXPECT_EQ(a.quality_score, b.quality_score);
  EXPECT_EQ(a.verdict, b.verdict);
  EXPECT_GE(a.quality_score, 0.0);
  EXPECT_LE(a.quality_score, 100.0);
  EXPECT_STREQ(QualityVerdictName(a.verdict),
               a.verdict == QualityVerdict::kFail    ? "fail"
               : a.verdict == QualityVerdict::kWarn  ? "warn"
                                                     : "pass");
}

TEST(QualityReportTest, DifferentRunsDiffer) {
  const auto a = EvaluateQuality("hash-a", "validate", R"({"config":{}})",
                                 {"ref-1"});
  const auto b = EvaluateQuality("hash-b", "validate", R"({"config":{}})",
                                 {"ref-1"});
  EXPECT_NE(a.quality_score, b.quality_score);
  EXPECT_NE(a.reprojection.rmse_px, b.reprojection.rmse_px);
}

TEST(QualityReportTest, InputRefsFeedTheSeed) {
  const auto a = EvaluateQuality("hash-a", "validate", R"({"config":{}})",
                                 {"ref-1"});
  const auto b = EvaluateQuality("hash-a", "validate", R"({"config":{}})",
                                 {"ref-2"});
  EXPECT_NE(a.quality_score, b.quality_score);
}

TEST(QualityReportTest, VerdictClassifiesByThresholds) {
  // The synthesized mock metrics top out below 99: a pass threshold of 99
  // forces a fail verdict.
  const auto strict = EvaluateQuality(
      "h", "validate",
      R"({"config":{"quality":{"thresholds":{"pass":99,"warn":98}}}})",
      {"ref-1"});
  EXPECT_EQ(strict.verdict, QualityVerdict::kFail);

  // pass=99, warn=0: the score always lands in [warn, pass) -> warn.
  const auto warn = EvaluateQuality(
      "h", "validate",
      R"({"config":{"quality":{"thresholds":{"pass":99,"warn":0}}}})",
      {"ref-1"});
  EXPECT_EQ(warn.verdict, QualityVerdict::kWarn);

  // pass=0: any score passes.
  const auto lenient = EvaluateQuality(
      "h", "validate",
      R"({"config":{"quality":{"thresholds":{"pass":0,"warn":0}}}})",
      {"ref-1"});
  EXPECT_EQ(lenient.verdict, QualityVerdict::kPass);
}

TEST(QualityReportTest, DefaultThresholdsAreApplied) {
  const auto report = EvaluateQuality("h", "validate", R"({"config":{}})",
                                      {"ref-1"});
  EXPECT_EQ(report.thresholds.pass, 80.0);
  EXPECT_EQ(report.thresholds.warn, 50.0);
}

TEST(QualityReportTest, JsonRoundTrip) {
  const auto report =
      EvaluateQuality("h", "validate", R"({"config":{}})", {"ref-1"});
  const auto parsed = QualityReportFromJson(QualityReportToJson(report));
  EXPECT_EQ(parsed.pipeline_hash, report.pipeline_hash);
  EXPECT_EQ(parsed.stage_id, report.stage_id);
  EXPECT_EQ(parsed.engine_name, report.engine_name);
  EXPECT_EQ(parsed.engine_version, report.engine_version);
  EXPECT_EQ(parsed.engine_git_commit, report.engine_git_commit);
  EXPECT_EQ(parsed.thresholds.pass, report.thresholds.pass);
  EXPECT_EQ(parsed.thresholds.warn, report.thresholds.warn);
  EXPECT_EQ(parsed.reprojection.rmse_px, report.reprojection.rmse_px);
  EXPECT_EQ(parsed.reprojection.mean_error_px,
            report.reprojection.mean_error_px);
  EXPECT_EQ(parsed.coverage.completeness_pct,
            report.coverage.completeness_pct);
  EXPECT_EQ(parsed.coverage.baseline_ratio, report.coverage.baseline_ratio);
  EXPECT_EQ(parsed.geometry.point_count, report.geometry.point_count);
  EXPECT_EQ(parsed.geometry.mean_confidence,
            report.geometry.mean_confidence);
  EXPECT_EQ(parsed.quality_score, report.quality_score);
  EXPECT_EQ(parsed.verdict, report.verdict);
  EXPECT_EQ(parsed.generated_at, report.generated_at);
}

TEST(QualityReportTest, VerdictNames) {
  EXPECT_STREQ(QualityVerdictName(QualityVerdict::kPass), "pass");
  EXPECT_STREQ(QualityVerdictName(QualityVerdict::kWarn), "warn");
  EXPECT_STREQ(QualityVerdictName(QualityVerdict::kFail), "fail");
}

// Schema Evolution Policy (RFC-0005 §5.5): new metric groups and keys are
// additive, so the schema must keep `additionalProperties` open on the
// metrics object and its groups while the top level stays closed.
TEST(QualityReportTest, SchemaAllowsAdditiveMetricGroups) {
  std::ifstream in(SPATIAL_QUALITY_SCHEMA_JSON);
  ASSERT_TRUE(in.good()) << "cannot open quality-report.schema.json";
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  // Top level is closed: unknown top-level keys are rejected.
  EXPECT_NE(text.find("\"additionalProperties\": false"), std::string::npos);

  // metrics and thresholds stay open for additive groups/keys.
  std::size_t open = 0;
  for (std::size_t pos = text.find("\"additionalProperties\": true");
       pos != std::string::npos;
       pos = text.find("\"additionalProperties\": true", pos + 1)) {
    ++open;
  }
  EXPECT_GE(open, 2u) << "metrics and thresholds must allow additive keys";

  // The frozen required field set (RFC-0005).
  EXPECT_NE(text.find("\"pipeline_hash\""), std::string::npos);
  EXPECT_NE(text.find("\"quality_engine\""), std::string::npos);
  EXPECT_NE(text.find("\"thresholds\""), std::string::npos);
  EXPECT_NE(text.find("\"metrics\""), std::string::npos);
  EXPECT_NE(text.find("\"quality_score\""), std::string::npos);
  EXPECT_NE(text.find("\"verdict\""), std::string::npos);
  EXPECT_NE(text.find("\"generated_at\""), std::string::npos);
  EXPECT_NE(text.find("\"pass\""), std::string::npos);
  EXPECT_NE(text.find("\"warn\""), std::string::npos);
  EXPECT_NE(text.find("\"fail\""), std::string::npos);
}

}  // namespace
}  // namespace spatial::engine
