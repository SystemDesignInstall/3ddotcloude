# ADR-027 — Adaptive Reconstruction Intelligence

- **Status:** accepted
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Users supply heterogeneous input (aerial image sets, terrestrial video, LiDAR sweeps, phone panoramas) and heterogeneous goals (high-accuracy metric survey, fast preview, photorealistic texture, gaussian splat). Asking users to hand-pick a matcher (SIFT, LightGlue, VGGT, LingBot-Map), depth estimator, meshing strategy, texturing mode, and gaussian generator is exactly the manual tuning the platform must remove. The backend registry (COLMAP, OpenMVS, Open3D, VGGT, gsplat, Nerfstudio, and others) contains overlapping capabilities, and AI models are priors only (ADR-006): their hypotheses must be validated by the classical core. The system therefore needs a selection layer that chooses algorithms from scene characteristics and user goals, records why, and stays auditable and overridable.

## Decision

The Adaptive Engine selects algorithms (matcher, depth, mesh, texture, gaussian generation) based on (a) scene characteristics and (b) user goals, never on manual settings. Scene characteristics include sensor mix, image count and resolution, image overlap, GNSS/LiDAR availability, texture richness, scene scale, and motion blur. User goals include accuracy budget, time budget, fidelity target, and requested output types. The engine produces an explicit strategy object: a set of capability selections (ADR-034), parameterized stages, and a human-readable rationale for every choice. Strategies are auditable and reproducible: they serialize into the recipe layer (ADR-026) and cache keys. The user can override any choice and the override is recorded as part of the strategy. Version 1 is a deterministic rule-based selector; learned or scored selection may be added later once benchmark evidence exists (ADR-029). AI hypotheses remain priors validated by the classical core; the Adaptive Engine never lets an AI output bypass classical validation.

**Deferred to:** post-M0 (Hybrid or SLAM milestone) — Adaptive Engine implementation; interfaces and the strategy schema are defined in P0.

## Alternatives

- **Expose all settings and require expert choice:** rejected — recreates the manual-tuning burden and contradicts the product direction.
- **Learned end-to-end selection from day one:** rejected — no training evidence base exists yet; rules are transparent, testable, and safe for the first versions.
- **Hard-code a single best pipeline:** rejected — fails on heterogeneous input and violates Principle 15 (every algorithm replaceable).

## Consequences

- Positive: users state goals, not parameters; strategy selection is auditable and explainable; rules can be tuned against benchmark results; overrides keep experts in control; AI capabilities are gated behind validation.
- Negative: strategy generation adds latency and complexity; rules must be maintained as new backends and capabilities arrive; mis-selection risk when scene characteristics are incomplete.
- Risks and mitigations: risk of silently degrading quality — mitigated by benchmark runs (ADR-029), strategy rationale surfaced to the user, and human override; risk of AI prior overreach — mitigated by the constitution rule that AI never produces authoritative geometry.

## References

- `docs/specifications/adaptive-engine.md`
- `docs/specifications/reconstruction-pipeline.md`
- ADR-006 (AI outputs are priors), ADR-026 (recipes), ADR-029 (benchmark framework), ADR-034 (capabilities), ADR-030 (quality engine)
