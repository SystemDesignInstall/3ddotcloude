#pragma once

// Quality engine for the QA pipeline stage (RFC-0005, quality-report
// specification). The terminal `validate` stage of a pipeline runs
// EvaluateQuality and stores the resulting report as a content-addressed
// artifact of type "quality_report"; the ExecutionManifest links it via
// quality_report_id (migration 0004). Evaluation is a pure function of the
// run (RFC-0005 §2): metrics, score, and verdict are deterministic and never
// depend on wall-clock time.

#include <cstdint>
#include <string>
#include <vector>

#include "engine/engine_common.h"
#include "engine/task/task_types.h"

namespace spatial::engine {

// Verdict classification of a run (RFC-0005 §5.3).
enum class QualityVerdict : int { kFail = 0, kWarn = 1, kPass = 2 };

const char* QualityVerdictName(QualityVerdict verdict) noexcept;

// Known metric groups (quality-report specification §3). Typed so the
// evaluator can normalize them against thresholds; the JSON schema stays open
// for additive extensions (RFC-0005 §5.5 Schema Evolution Policy).
struct ReprojectionMetrics {
  double rmse_px = 0.0;        // root-mean-square reprojection error (px)
  double mean_error_px = 0.0;  // mean reprojection error (px)
};

struct CoverageMetrics {
  double completeness_pct = 0.0;  // fraction of expected extent with output (%)
  double baseline_ratio = 0.0;    // effective baseline to scene-depth ratio
};

struct GeometryMetrics {
  double point_count = 0.0;      // reconstructed point count
  double mean_confidence = 0.0;  // mean keypoint confidence (0-1)
};

// Effective quality thresholds (RFC-0005 §5.3; defaults pass=80, warn=50).
// Configurable via config.quality.thresholds.{pass,warn}.
struct QualityThresholds {
  double pass = 80.0;
  double warn = 50.0;
};

struct QualityReport {
  std::string pipeline_hash;
  std::string stage_id;  // "validate"
  std::string engine_name;
  std::string engine_version;
  std::string engine_git_commit;
  QualityThresholds thresholds;
  ReprojectionMetrics reprojection;
  CoverageMetrics coverage;
  GeometryMetrics geometry;
  double quality_score = 0.0;  // aggregate 0-100
  QualityVerdict verdict = QualityVerdict::kFail;
  std::string generated_at;  // ISO-8601 UTC
};

// Deterministic evaluation (RFC-0005 §2). `config_json` is the effective
// stage configuration (may carry config.quality.thresholds.{pass,warn});
// `input_refs` are the real CAS content hashes consumed by the validate
// stage and are part of the evaluation seed. Returns the completed report.
QualityReport EvaluateQuality(const std::string& pipeline_hash,
                              const std::string& stage_id,
                              const std::string& config_json,
                              const std::vector<ArtifactRef>& input_refs);

// Renders the report as canonical JSON (quality-report.schema.json).
std::string QualityReportToJson(const QualityReport& report);

// Parses a canonical JSON report; throws ProjectError on malformed content.
QualityReport QualityReportFromJson(const std::string& json);

}  // namespace spatial::engine
