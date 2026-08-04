# ADR-013 — Plugin and Adapter Strategy

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform must integrate a wide range of third-party and in-house algorithms (COLMAP, OpenMVS, Open3D, GTSAM, Ceres, KISS-ICP, VGGT, gsplat, Nerfstudio, and others), some under commercial-license or research-only terms. These backends have incompatible build systems, data models, and licensing. The platform also needs the capability system (SparseReconstruction, DenseStereo, BundleAdjustment, ICP, SurfaceReconstruction, Texturing, GaussianGeneration, LidarOdometry, LoopClosure, GnssIntegration) to remain extensible without forking Core. Backends are declared in the THIRD_PARTY.yml registry with a lifecycle status; several are still "planned". We must separate what is stable and Core-owned from what is replaceable and backend-specific.

## Decision

The plugin chain is strictly layered: Core -> PluginManager -> Plugin -> Adapter -> Algorithm. Core defines the stable Plugin API in `core/plugin/**`. A plugin is a unit of distribution that registers one or more adapters; an adapter is the unit of capability that wraps a concrete algorithm. Adapters declare capabilities via the Capability API; the engine selects adapters purely by capability, never by vendor name or hard-coded path. Plugins may only interact with Core through the public Plugin API and must never bypass Core to touch storage, the scene, artifacts, or the scheduler directly. Mock adapters (ADR-021) implement every ProcessingAdapter interface and are used for contract validation and integration tests. Dynamic loading of `.so`/`.dll` is deferred, but interfaces and registration are designed so that dynamic loading can be introduced behind the PluginManager without changing adapter or Core code. Licensing gates (e.g. VGGT commercial license, MASt3R/DUSt3R research-only) are declared in the plugin manifest and enforced at load/selection time.

## Alternatives

- **Dynamic loading from day one (dlopen/LoadLibrary):** rejected because it couples build and packaging (ABI, dependency resolution) before the adapter contract is stable; deferred behind the PluginManager.
- **Plugins as separate processes:** rejected for interactive latency and overhead; the Worker Protocol already provides process isolation for heavy algorithms.
- **Monolithic Core with in-tree backends:** rejected because it violates the principle that every algorithm is replaceable and would couple the kernel to vendor code and licensing.

## Consequences

- Positive: Core surface is small and stable; capability negotiation decouples feature selection from backend identity; new algorithms ship as plugins without kernel changes; licensing gates are enforceable at selection time; mock adapters make the whole chain testable without any backend installed.
- Negative: indirection costs and an extra layer to understand; capability metadata must be kept accurate or the engine will mis-select; deferred dynamic loading means the first plugins are linked statically.
- Risks and mitigations: risk of adapter contracts drifting from real backend behavior — mitigated by mock adapters validated against golden outputs and by an architecture-review gate; risk of plugins circumventing Core — mitigated by constitution protection of `core/plugin/**` and code review.

## References

- `docs/specifications/plugin-api.md`
- `docs/specifications/task-model.md`
- `docs/architecture/process-model.md`
- ADR-021 (mock adapters and interface isolation)
- ADR-016 (testing strategy)
- ADR-031 (M0 scope — mock adapters and demo worker in scope)
