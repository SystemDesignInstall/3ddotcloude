# ADR-030 — Quality Engine as First-Class

- **Status:** accepted
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Reconstruction outputs need quantifiable quality signals: coverage of the scene, camera baseline quality, view angles, texture resolution, expected geometric error, expected mesh quality, and expected gaussian quality. Today these are derived ad hoc by tools or left to subjective visual inspection. Principle 14 makes quality metadata first-class data: uncertainty, covariance, confidence, and residuals are carried with the geometry (ADR-025), not computed on demand. Without a Quality Engine, users cannot decide "is this scene good enough to deliver?" and the Adaptive Engine (ADR-027) cannot evaluate its own choices.

## Decision

A Quality Engine provides composable, dataset-agnostic quality measures over a Scene: coverage, baseline sufficiency, view angle distribution, texture resolution, expected error, expected mesh quality, and expected gaussian quality. Each measure consumes first-class data already present on the scene (per-point confidence and covariance from ADR-025, observation poses, calibration, and geometry frames) and produces typed results with units and explicit meaning. Results are stored as quality metadata attached to the scene, so quality itself becomes queryable through the Scene Query API (ADR-035) and comparable across versions via the workflow (ADR-028). Quality measures are invoked by the workflow Review stage, surfaced to the user for the Improve stage, and consumed by the Adaptive Engine as part of its scoring. Measures are classical and deterministic; they never rely on AI inference for authority.

**Deferred to:** post-M0 (Photogrammetry or Hybrid milestone) — Quality Engine implementation; quality result schemas and the measure taxonomy are defined in P0.

## Alternatives

- **Quality as ad-hoc per-tool calculations:** rejected — no shared meaning, no comparability across versions or recipes, and no automation surface.
- **A single composite quality score:** rejected — hides what failed; users and the adaptive engine need per-dimension signals.
- **AI-judged quality:** rejected — violates the AI-never-authoritative rule (ADR-006); quality judgments must be reproducible.

## Consequences

- Positive: deliverables are objectively judged; the Improve stage becomes data-driven; the adaptive engine gets measurable feedback; quality reports are comparable across runs and versions.
- Negative: quality measures must be maintained as geometry types grow; some measures (e.g. expected mesh quality) depend on production parameters that must be pinned; measure calibration takes effort.
- Risks and mitigations: risk of meaningless aggregate numbers — mitigated by per-dimension results and explicit units; risk of measures diverging from user-perceived quality — mitigated by benchmark validation (ADR-029) and a calibration loop in the Review stage.

## References

- `docs/specifications/scene-model.md`
- `docs/specifications/geometry-model.md`
- ADR-025 (per-point provenance and uncertainty), ADR-027 (adaptive engine), ADR-028 (workflow), ADR-029 (benchmarks), ADR-035 (scene query API)
