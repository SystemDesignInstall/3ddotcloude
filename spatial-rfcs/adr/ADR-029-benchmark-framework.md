# ADR-029 — Benchmark Framework as Product Evidence Base

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

The platform competes directly with RealityCapture and Metashape on reconstruction quality and throughput. Claims such as "comparable F-score on Tanks and Temples" or "2× faster than RealityCapture" must be evidence-backed, reproducible, and comparable across releases. Reconstruction accuracy is dataset- and metric-specific (accuracy, completeness, F-score, ATE, RPE), and runtime depends on hardware and configuration (runtime, peak RAM/VRAM, throughput, PSNR for textures). Today there is no shared registry of datasets, licenses, checksums, or a canonical metric taxonomy, so benchmark results cannot be trusted or repeated.

## Decision

A benchmark framework is the product's evidence base. A versioned benchmark datasets registry defines the supported public datasets — ETH3D, Tanks and Temples, DTU, KITTI, TUM RGB-D, Replica, ScanNet, MegaDepth, BlendedMVS — each with provenance, license reference (maintained in `DATASET_LICENSES.yml`), download instructions, and content checksums. A metrics taxonomy defines canonical metrics: accuracy, completeness, F-score, ATE, RPE, runtime, peak RAM/VRAM, throughput, and texture PSNR, each with a precise definition and reference implementation. Runs are automated: a benchmark run resolves a recipe (ADR-026) against a dataset slice on a declared hardware profile and produces a machine-readable JSON report (inputs, dataset checksums, recipe, engine version, git commit, metrics, environment). Reports are archived immutably and ingested by CI for regression comparison, and they feed the Adaptive Engine's strategy evidence (ADR-027) and public marketing evidence. The registry and schemas are ratified in P0; the harness ships later.

**Deferred to:** post-M0 (Photogrammetry milestone) — the runnable benchmark harness and CI regression gates.

## Alternatives

- **Ad-hoc comparisons during development:** rejected — results are unreproducible, unlicensed, and not citable; cannot back product claims.
- **Single-score aggregated metric:** rejected — hides dataset-specific failure modes and misleads quality engineering.
- **Internal-only datasets:** rejected — cannot be published as evidence against competitors and licensing is not controlled.

## Consequences

- Positive: product claims become auditable; regressions are caught in CI; dataset licensing and checksums are controlled; results inform adaptive selection and roadmap prioritization.
- Negative: datasets are large and downloads must be managed; metric reference implementations must be maintained; benchmark runs are slow and need hardware profiles.
- Risks and mitigations: risk of license violations on benchmark data — mitigated by `DATASET_LICENSES.yml` and gating on dataset download; risk of overfitting to benchmark sets — mitigated by withholding a reserved subset; risk of metric drift — mitigated by versioned reference metric implementations.

## References

- `docs/benchmarks/benchmark-framework.md`
- `docs/specifications/artifact-format.md`
- ADR-026 (recipes), ADR-027 (adaptive intelligence), ADR-037 (public API stability — benchmark reports as public evidence)
