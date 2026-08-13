# RFC-0005 — Accuracy & Quality Assurance

- **Status:** ratified
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-07
- **Supersedes:** none
- **Depends on:** RFC-0003, ADR-010, ADR-020, ADR-021, ADR-030, ADR-034
- **Protected surfaces touched:** Engine Execution (`engine/**` — new `engine/pipeline/quality/**` module, `ExecutionManifest.quality_report_id` wiring), Capability API (`schemas/**` — new `quality-report.schema.json`), Artifact Format (`core/artifacts/**` — `quality_report` artifact kind), CLI (`cli/**` — `spatial report`)

## Summary

RFC-0005 makes Accuracy & Quality Assurance a **first-class pipeline stage** — the `validate`/`Review` stage of the Processing Engine (RFC-0003 P1.4) becomes a real quality gate. It ratifies the **QualityReport** artifact format (hierarchical, extensible metrics; engine provenance; `quality_score` + `verdict`), the binding of a completed run to its report via `ExecutionManifest.quality_report_id` (migration 0004 column), and the `spatial report` CLI surface. No new execution mechanisms are introduced: QA runs as an ordinary deterministic stage through the existing PipelineCompiler → TaskGraph → Scheduler → CAS path. Full Scene-level quality measures remain deferred to ADR-030 post-M0; RFC-0005 ratifies the manifest-level QA stage and the report format those measures will fill.

## Motivation

P1.4 completed the Processing Engine MVP: a pipeline compiles to a TaskGraph, executes through the scheduler, and persists an `ExecutionManifest` — but the pipeline's final `validate` stage is a no-op echo. Users and the Adaptive Engine (ADR-027) cannot yet answer "is this scene good enough to deliver?", and there is no machine-readable record of quality against which future engine versions can be compared (Principle 6, reproducibility). ADR-030 ratifies a Quality Engine over the Scene and defers it post-M0; the manifest-level stage and report format are the portion of that intent that can land now without the C++ Scene layer, and they define the interface that the Scene-level measures will implement unchanged.

## Design

### 5.1 QA as an ordinary pipeline stage

No scheduler, engine, or worker changes are introduced. The `validate` stage (capability `validation`, output artifact kind `quality_report`) is already part of the mock photogrammetry pipeline. RFC-0005 makes its worker behavior real: on `task_type == "validate"` the in-process mock emits a **QualityReport artifact** — deterministic JSON registered in CAS (ADR-010) like any other task output — instead of an opaque payload. A future COLMAP/OpenMVG/BA worker implements the same contract; the interface does not change.

```
Images → Feature Extraction → Reconstruction → Validate → QualityReport artifact
                                                           │
                                    ExecutionManifest.quality_report_id ──┘
```

### 5.2 QualityReport artifact (`quality-report.schema.json`)

The report is an immutable, content-addressed JSON artifact. It is P0-frozen in the same spirit as `benchmark-report.schema.json`: forward-compatible, not backward-changed.

```json
{
  "pipeline_hash": "<sha256 of the pipeline run identity>",
  "stage_id": "validate",
  "quality_engine": { "name": "quality-engine", "version": "0.1.0", "git_commit": "<engine git commit>" },
  "thresholds": {
    "reprojection": { "rmse_px": 1.0 },
    "coverage": { "completeness_pct": 90.0 },
    "score": { "pass": 80, "warn": 50 }
  },
  "metrics": {
    "reprojection": { "rmse_px": 0.81, "mean_error_px": 0.62 },
    "coverage": { "completeness_pct": 94.2, "baseline_ratio": 0.71 },
    "geometry": { "point_count": 128400, "mean_confidence": 0.87 }
  },
  "quality_score": 87,
  "verdict": "pass",
  "generated_at": "<ISO 8601 UTC>"
}
```

- **`metrics`** is hierarchical and extensible: known groups (`reprojection`, `coverage`, `geometry`) are typed in C++ (`engine/pipeline/quality`), and the JSON schema permits additional groups and keys. Future metrics (GCP accuracy, scale error, ICP residual, mesh deviation, LiDAR registration, confidence maps, bundle covariance) are added as new groups or keys with **no schema migration**.
- **`quality_engine`** pins the provenance of the quality computation itself (name/version/git commit), enabling cross-version comparison of reports.
- **`quality_score`** (0–100) is an independent deterministic aggregate; **`verdict`** (`pass`/`warn`/`fail`) is its classification against the `score` thresholds. The two are kept separate so automation can act on the score and UI can present a verdict.
- **`thresholds`** is a snapshot of the effective thresholds (from the run configuration with documented defaults), so the report is self-contained and auditable later.

### 5.3 Score and verdict

- The demo evaluator derives metrics deterministically from the stage's input artifact hashes (identical inputs → identical report, preserving AC-8 cache semantics).
- `quality_score` = weighted, threshold-normalized combination of the known metrics, clamped to [0, 100].
- `verdict`: `pass` when `score ≥ pass_threshold`, `warn` when `score ≥ warn_threshold`, else `fail`. Defaults `pass = 80`, `warn = 50`; overridable via the run configuration (`config.quality.thresholds.score`).

### 5.4 Manifest binding

`ExecutionManifest.quality_report_id` (column already present in migration 0004) is the **artifact uuid** of the QualityReport, not a content hash. After a run, the engine resolves the `quality_report` stage's produced content hash → artifact uuid (ADR-010 manifest index) and persists it. Cache-hit replay reproduces the same content hash → same uuid, so the id is stable across cache-hit reruns (AC-8). Pipelines without a `quality_report` stage leave the field null.

### 5.5 CLI

`spatial report <run-id> [--project <dir>]` loads the manifest, resolves `quality_report_id`, reads the report artifact from CAS, and prints its JSON. `spatial status` already renders `quality_report_id` in the manifest.

## Schema Evolution Policy

The `quality-report.schema.json` format is frozen as an **immutable, extensible** contract:

1. **Semantics never change in place.** An existing metric or key never changes meaning or units; a field never changes type.
2. **New metrics are additive only**: a new group, or a new key inside an existing group, with `additionalProperties` at both levels.
3. **Removal or renaming of existing keys** is allowed only through a new **major** schema version (a new `$id`); the old version remains valid for historical artifacts.
4. **Unknown groups and keys are ignored** by report consumers unless consumed. A consumer must never fail on an unknown key.

## Compatibility

- No breaking change: `quality_report_id` is already a column in migration 0004 (reserved for this RFC); existing manifests persist with the field null until a QA stage runs.
- Artifact and manifest schema contracts are unchanged; only a new artifact kind (`quality_report`) and one new JSON schema are added.
- The comment "reserved for RFC-0004 (Accuracy)" in `execution_manifest.h` and migration 0004 is updated to reference RFC-0005; RFC-0004 remains reserved for the plugin/worker ecosystem per RFC-0003.

## Alternatives

- **A separate QA subsystem** outside the pipeline: rejected — QA is a normal stage (RFC-0003), and a parallel subsystem would duplicate scheduling, caching, and provenance.
- **Flat metrics map** (`{"rmse_px": ...}`): rejected — a flat map breaks on every metric taxonomy addition and provides no grouping for thresholds.
- **`quality_score` only, no verdict:** rejected — automation needs a number and users need a class; the two are complementary.
- **Verdict-only:** rejected — hides the magnitude of quality (a `pass` of 61 vs 99 is materially different).

## Open Questions

- Calibration of the demo score formula against benchmark datasets (ADR-029) happens when real reconstruction metrics arrive; the formula is contained in `engine/pipeline/quality` and does not change the report contract.
- Whether `warn` requires per-metric (not just aggregate) thresholds is deferred to the Scene-level Quality Engine (ADR-030).

## Impact

- **Modules:** new `engine/pipeline/quality/**` (evaluator + report model), `engine/workers/mock_pipeline_runner.cpp` (validate branch), `engine/pipeline/execution_manifest.{h,cpp}` (`SetQualityReportId`), `engine/engine.cpp` (binding), `cli/main.cpp` (`spatial report`).
- **Schemas:** `schemas/json/quality-report.schema.json`; migration 0004 column already present (comment updated).
- **Docs:** `docs/specifications/quality-report.md` (spec), governance README index.
- **Tests:** `tests/unit/test_quality_report.cpp`, additions to `tests/unit/test_mock_pipeline_e2e.cpp`; schema-extensibility test through the JSON-schema gate.
- **Acceptance:** a mock pipeline run produces a schema-valid QualityReport, the manifest references it, `spatial report` prints it, and a cache-hit rerun reproduces the same `quality_report_id`.
