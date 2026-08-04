# ADR-026 — Recipes as Versioned Pipeline Definitions

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Users think in terms of named, repeatable outcomes ("reconstruct by recipe RC High Accuracy"), not raw parameter dumps. Today a pipeline is configured with an arbitrary JSON configuration object; two runs with identical configuration text can still differ if the producer version or code commit changes. The scheduler cache (ADR-020) already keys on input hashes, config hash, producer version, and git commit, but that key is opaque to users and cannot be shared, reviewed, or versioned as a unit. We need a curated layer on top of raw configuration: named, versioned, immutable pipeline definitions that users can cite, compare, and trust.

## Decision

A Recipe is an immutable, versioned pipeline definition consisting of: `name`, `version`, `git_commit`, a JSON Schema for its parameters, an ordered list of `stages`, validated parameters, and a `cache_identity`. Recipes are validated against `schemas/**` at load time and stored in a recipe library that is itself versioned. The recipe's cache identity folds into the scheduler cache key so that a run is reproducible from recipe name + version + inputs alone. Recipes are curated, named configurations: the raw config hash remains, but it is derived from the recipe, never hand-authored. Canonical recipe families are defined for "Reconstruction", "Matching", "Optimization", "Mesh", and "Texture"; a recipe of a family declares which stage capabilities it requires, letting the engine select implementations by capability (ADR-034). The schema and library layout are ratified in P0; implementation ships later.

**Deferred to:** post-M0 (Photogrammetry milestone) — recipe library storage, stage orchestration from a recipe, and recipe-driven CLI.

## Alternatives

- **Raw configuration files only:** rejected — no stable identity, no sharing, no comparison, and no cache identity users can reason about.
- **Fixed, hard-coded pipelines:** rejected — violates Principle 15 (every algorithm replaceable) and the adaptive engine goals (ADR-027).
- **Mutable recipes:** rejected — an edited recipe would silently change provenance and break the reproducibility contract.

## Consequences

- Positive: reproducible runs are citable and auditable; recipes become the user-facing contract for reconstruction quality; cache hits and reuse are explainable; recipe families map naturally to user goals.
- Negative: an extra layer of indirection over configuration; recipes must be curated and versioned or they rot; schema evolution of a recipe family must be coordinated.
- Risks and mitigations: risk of recipe version skew with engine capabilities — mitigated by required-capability declarations and selection-time validation; risk of recipe drift from the code that produced it — mitigated by the `git_commit` field and scheduler cache key.

## References

- `docs/specifications/reconstruction-pipeline.md`
- `docs/specifications/task-model.md`
- ADR-020 (scheduler persistence and task cache), ADR-027 (adaptive intelligence), ADR-031 (M0 scope), ADR-034 (capability plugin architecture)
