# ADR-034 — Capability-Based Plugin Architecture

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

The backend registry contains overlapping tools with incompatible names and licenses: COLMAP and OpenMVS both do dense stereo; gsplat, Nerfstudio, and Open3D all touch gaussian generation; VGGT is commercial-license gated and MASt3R/DUSt3R research-only. Principle 8 (capabilities over names) says components declare what they can do, not which tool they are, and the engine selects implementations by capability. ADR-013 fixed the chain Core → PluginManager → Plugin → Adapter → Algorithm; this ADR fixes what an adapter advertises so selection can be meaningful, automated (ADR-027), and licensing-safe (ADR-029).

## Decision

An AdapterDescriptor is the machine-readable contract an adapter publishes: its declared capabilities, input and output schema references, and a license reference resolved against THIRD_PARTY.yml. The capability taxonomy is the single extensible vocabulary: SparseReconstruction, DenseStereo, BundleAdjustment, ICP, SurfaceReconstruction, Texturing, GaussianGeneration, LidarOdometry, LoopClosure, GnssIntegration. Capabilities carry typed parameter and IO schemas so the engine can validate a selected implementation against a recipe stage (ADR-026) before running it. The engine never selects by vendor or name: it builds the set of adapters whose capabilities satisfy the requested stage and applies license gates and mock-vs-real policy. Mock adapters (ADR-021) declare the same capabilities as their real counterparts, so the full pipeline is testable with no backend installed. Licensing gates (e.g. VGGT commercial, MASt3R/DUSt3R research-only) are enforced at load/selection time, not at build time. Interfaces and descriptor schema are in M0; dynamic loading of plugin binaries is deferred but the PluginManager contract is designed for it.

**Deferred to:** post-M0 — dynamic loading of `.so`/`.dll` plugins; M0 ships the descriptor schema, capability taxonomy, mock plugins, and static registration.

## Alternatives

- **Select adapters by vendor name or hard-coded path:** rejected — violates Principle 8 and couples the engine to every backend forever.
- **One super-adapter per vendor:** rejected — hides what a tool can actually do and makes replacement impossible at the stage level.
- **No license enforcement at selection time:** rejected — VGGT and research-only backends must never silently run in a commercial deployment.

## Consequences

- Positive: algorithms are swappable without engine changes (Principle 15); the adaptive engine (ADR-027) and recipes (ADR-026) drive selection uniformly; licensing risk is enforced mechanically; mocks and real adapters are interchangeable in tests.
- Negative: descriptors must be kept accurate or the engine mis-selects; capability taxonomy must grow carefully through the RFC process; static registration means the first plugins are linked, not dropped in.
- Risks and mitigations: risk of descriptor drift from real backend behavior — mitigated by golden-output validation of adapters and architecture review of `core/plugin/**` and `schemas/**`; risk of taxonomy bloat — mitigated by the constitution-protected Capability API and RFC change control; risk of license bypass — mitigated by enforcement at selection time plus plugin manifest review.

## References

- `docs/specifications/plugin-api.md`
- `docs/specifications/task-model.md`
- ADR-013 (plugin and adapter strategy), ADR-021 (mock adapters), ADR-026 (recipes), ADR-027 (adaptive engine), ADR-031 (M0 scope)
