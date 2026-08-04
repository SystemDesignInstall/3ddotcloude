# Recipe Model Specification

- **Status:** ratified (P0) — **implementation deferred** (ADR-026, ADR-031)
- **References:** ADR-026 (recipes as versioned pipeline definitions), ADR-020 (scheduler cache), ADR-034 (capability architecture), ADR-027 (adaptive engine)
- **Protected surface:** recipe schema under `schemas/**` (Constitution §2)
- **Scope:** the data model and schema are **fixed now**; recipe library storage, stage orchestration, and recipe-driven CLI ship post-M0 (Photogrammetry milestone).

## 1. Purpose

Users think in terms of named, repeatable outcomes — "reconstruct by recipe **RC High Accuracy**" — not raw parameter dumps. A Recipe is the curated, named, versioned, immutable layer on top of raw configuration: citable, comparable, and reproducible as a unit. Raw config hashes remain, but they are **derived from the recipe, never hand-authored**.

## 2. The Recipe

```
Recipe = {
  id,                 # Uuid — stable identity, immutable
  name,               # string — display name, e.g. "RC High Accuracy"
  version,            # semver — bumps create a NEW recipe; old versions stay executable forever
  git_commit,         # pins the exact engine code the recipe was authored against
  config_schema,      # JSON Schema for the recipe's parameters
  stages[],           # ordered stage list (each: stage kind, capability constraint, quality target)
  params,             # typed parameters per stage, validated against config_schema
  cache_identity      # folds into the scheduler cache key
}
```

Rules:

1. **Immutable once published.** An edited recipe would silently change provenance and break the reproducibility contract. `version` bumps create a new Recipe; the old one remains executable forever.
2. **`git_commit` is mandatory.** It pins the engine code the recipe was authored against, mitigating recipe drift from the code that produced it.
3. **Validated at load time** against `schemas/**`; schema and library layout are ratified in P0.
4. **Stage capabilities are declared, not bound.** Each stage names the capability it requires; the engine selects implementations by capability at selection time (ADR-034).

## 3. Named curated recipes

- Curated, named configurations are the user-facing contract for reconstruction quality. Canonical example: **RC High Accuracy**.
- Canonical **recipe families**: `Reconstruction`, `Matching`, `Optimization`, `Mesh`, `Texture`. A recipe of a family declares which stage capabilities it requires, letting the engine select implementations by capability and letting the adaptive engine choose among candidate recipes by predicted quality/cost (ADR-027).
- Curated recipes live in a **recipe library** that is itself versioned; the library is the sharing and review unit users reason about.

## 4. Recipe library

- Stored and versioned as a unit; recipes are citeable ("RC High Accuracy v2.1") and comparable.
- Library version skew with engine capabilities is mitigated by required-capability declarations and selection-time validation.

## 5. Relationship to task cache keys (ADR-026, ADR-020)

The recipe's `cache_identity` **folds into the scheduler cache key**:

```
cache_key = hash( input artifact hashes
                + configuration hash (derived from the recipe, never hand-authored)
                + producer/algorithm version
                + git commit (of the producing code, matched to recipe.git_commit) )
```

Consequences:

1. A run is reproducible from **recipe name + version + inputs alone**; equal keys always reuse the same cached outputs.
2. Any change to a key component invalidates the entry — including a recipe version bump (new `cache_identity`) and an engine code change (new `git_commit`).
3. Cache hits and misses are recorded as structured events with the key, so reproducibility is auditable.
4. A cache hit is recorded in provenance as a synthetic producer (recipe + git commit + cache identity).
5. Every stage declares `deterministic` (`true`/`false`); deterministic stages are cached and may be skipped on a hit; non-deterministic stages are never replayed from cache without explicit opt-in.

## 6. Deferred scope

- Recipe library storage, stage orchestration from a recipe, recipe-driven CLI, adaptive recipe selection → **post-M0**.
- The data model, schema (`schemas/json/recipe.schema.json`), and cache-key relationship are **fixed in P0** and must not require migration.

## 7. Invariants

1. Recipes are immutable once published; only version bumps create new recipes.
2. `cache_identity` and `git_commit` are always present and always fold into the cache key.
3. Every stage declares a capability constraint and a quality target.
4. Parameters validate against `config_schema` at load time.

## References

- `docs/specifications/reconstruction-pipeline.md` (§3 Recipes, §4 Reproducibility)
- `docs/specifications/task-model.md`
- `docs/architecture/data-flow.md` (cache short-circuit at the scheduler)
- ADR-020, ADR-026, ADR-027, ADR-031, ADR-034
