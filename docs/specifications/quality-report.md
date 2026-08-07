# Quality Report Specification

Status: Ratified
References: RFC-0005 (accuracy & quality assurance), ADR-030 (Scene-level QA, deferred post-M0), ADR-009 (artifact store)

This document defines the Quality Report: the output of the QA stage (`validate`) in a spatial pipeline, produced by the quality engine. It complements `reconstruction-pipeline.md` (QA as a pipeline stage) and is written to the artifact store as a content-addressed artifact whose manifest type is `quality_report`.

## 1. Role

Quality Assurance is a regular pipeline stage (RFC-0005 §5.1). The final stage of a pipeline is `validate`; it runs the quality engine against the pipeline's outputs and produces a Quality Report. The report:

- is a deterministic function of the pipeline run (inputs, configuration, outputs, and engine version), so re-running a run reproduces the report;
- classifies the run with a verdict (`pass` / `warn` / `fail`) driven by an aggregate `quality_score` (0–100);
- is linked from the pipeline's `ExecutionManifest.quality_report_id`.

A `fail` verdict marks the pipeline result as failed even if every producing stage completed.

## 2. Determinism

The quality engine is deterministic: given the same inputs, configuration, outputs, engine version, and engine git commit, it produces the same metrics, the same `quality_score`, and the same `verdict`. No wall-clock-derived randomness is used; the seeded PRNG (when the engine needs randomness) is seeded from the input hashes so that the result is reproducible across runs and machines.

## 3. Metrics model

`metrics` is a hierarchical object. Each key is a metric group; each group maps metric keys to numeric values. Groups are additive-only under the RFC-0005 Schema Evolution Policy.

Known groups produced by the P1.5 engine:

| Group | Key | Unit | Meaning |
| --- | --- | --- | --- |
| `reprojection` | `rmse_px` | px | Root-mean-square reprojection error. |
| `reprojection` | `mean_error_px` | px | Mean reprojection error. |
| `coverage` | `completeness_pct` | % | Fraction of expected scene extent with output. |
| `coverage` | `baseline_ratio` | — | Effective baseline to scene-depth ratio. |
| `geometry` | `point_count` | count | Number of reconstructed points. |
| `geometry` | `mean_confidence` | 0–1 | Mean keypoint confidence. |

Additional groups and keys are permitted and ignored by consumers unless consumed.

## 4. Thresholds and verdict

The quality engine classifies the run with a verdict derived from an aggregate score:

| Verdict | Condition (defaults) |
| --- | --- |
| `pass` | `quality_score >= 80` |
| `warn` | `80 > quality_score >= 50` |
| `fail` | `quality_score < 50` |

Thresholds are configurable through the task configuration (`config.quality.thresholds`); when absent, the defaults above apply. The effective thresholds are snapshotted into the report's `thresholds` field so a historical report can be interpreted even if defaults change.

The aggregate `quality_score` is a weighted combination of the known metric groups, each normalized to a 0–100 sub-score against its threshold. Weights and normalization for post-M0 metric groups are defined by the Scene-level quality engine (ADR-030); the P1.5 engine implements the manifest-level stage, the schema, and deterministic evaluation of the known groups.

## 5. Report format

The report payload is a single JSON object conforming to `schemas/json/quality-report.schema.json`. Fields:

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `pipeline_hash` | string | yes | Identity of the pipeline run (RFC-0003 AC-8). |
| `stage_id` | string | yes | Stage id, `validate` for the QA stage. |
| `quality_engine` | object | yes | `{ name, version, git_commit }` of the producing engine. |
| `thresholds` | object | yes | Snapshot of effective thresholds, grouped like metrics. |
| `metrics` | object | yes | Hierarchical metric groups (Section 3). |
| `quality_score` | number | yes | Aggregate score, 0–100. |
| `verdict` | string | yes | One of `pass`, `warn`, `fail`. |
| `generated_at` | string | yes | ISO-8601 UTC timestamp. |

Example:

```json
{
  "pipeline_hash": "7f3a...",
  "stage_id": "validate",
  "quality_engine": { "name": "spatial-platform", "version": "0.1.0", "git_commit": "deadbeef" },
  "thresholds": { "quality_score": { "pass": 80, "warn": 50 } },
  "metrics": {
    "reprojection": { "rmse_px": 0.9, "mean_error_px": 0.7 },
    "coverage": { "completeness_pct": 92.0, "baseline_ratio": 0.31 },
    "geometry": { "point_count": 41280, "mean_confidence": 0.87 }
  },
  "quality_score": 87.5,
  "verdict": "pass",
  "generated_at": "2026-08-07T09:00:00Z"
}
```

## 6. Integration

The report is produced by the `validate` stage and stored as an artifact of type `quality_report`. Its artifact hash is recorded in `ExecutionManifest.quality_report_id`. Consumers:

- the CLI (`spatial report`) renders the report and the verdict for a run;
- the engine treats a `fail` verdict as a failed run regardless of stage completion.

## 7. Schema Evolution Policy

The report schema evolves under RFC-0005 §5.5:

1. New metric groups and keys are additive: the schema's `additionalProperties` are open, and consumers must ignore unknown keys unless explicitly consumed.
2. Values of existing keys never change unit or meaning.
3. Required fields are never removed.
4. `quality_engine` and `thresholds` are always snapshotted so historical reports remain interpretable.

All changes go through RFC-0005 review.
