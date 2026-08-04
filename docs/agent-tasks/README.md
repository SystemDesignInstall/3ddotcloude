# Agent Task Decomposition

- **Status:** ratified (P0)
- **References:** CONSTITUTION.md, ADR-001 (monorepo), ADR-031 (M0 scope), ADR-037 (public API stability)
- **Purpose:** parallel-agent work plan. Teams are **domain-owned**, build order is milestone-driven, and protected surfaces change only through RFC.

## 1. Domain teams

### Core Platform
Owns the engineering skeleton: `.spx` project format, artifact store, typed errors, scheduler, workers, CLI.

- `core/project`, `core/storage`, `core/artifacts`, `core/errors`
- `engine/scheduler`, `engine/workers`
- `cli`

### Geometry
Owns the scene and coordinate model: Scene graphs, strict coordinates, sensors, calibration.

- `core/scene`, `core/coordinates`, `core/geometry`, `core/sensors`, `core/calibration`

### Photogrammetry
Owns image/video reconstruction: COLMAP and OpenMVS adapters, photogrammetry pipelines, image/video importers.

- `adapters/colmap`, `adapters/openmvs`
- `pipelines/photogrammetry`
- `importers/images`, `importers/video`

### SLAM
Owns live/online reconstruction: KISS-ICP and GTSAM adapters, LiDAR SLAM pipelines, LiDAR/E57/rosbag importers.

- `adapters/kiss_icp`, `adapters/gtsam`
- `pipelines/lidar_slam`
- `importers/las`, `importers/e57`, `importers/rosbag`

### AI
Owns learned priors and AI workers: VGGT and neural adapters, Gaussian/NeRF backends, AI worker processes, adaptive intelligence.

- `adapters/vggt`, `adapters/lingbot_map`, `adapters/gsplat`, `adapters/nerfstudio`
- `python/ai_workers`, `engine/intelligence`

### UX
Owns the human surfaces: UI, SDK, services, viewer, benchmarks.

- `ui`, `python/spatial_sdk`, `services`, `viewer`, `benchmarks`

## 2. Coordination rules

1. **Ownership = no cross-team edits without RFC.** A team edits only the paths it owns. Touching another team's paths requires a ratified RFC and Architecture Review; ad hoc cross-team edits are rejected by `constitution-check`.
2. **Contracts are frozen in P0.** Schemas (`schemas/**`), strict types (`core/coordinates/**`), the Scene model, the Plugin/Capability APIs, and error codes are fixed in P0. Teams build **against the frozen contract**; no team may reshape a contract to fit its milestone. Contract change requires RFC, not negotiation.
3. **Protected paths require RFC.** Any modification of Constitution-protected surfaces (`core/coordinates/**`, `core/geometry/**`, `core/scene/**`, `core/scene/observation_graph/**`, `core/scene/geometry/**`, `core/artifacts/**`, `core/plugin/**`, `schemas/**`) must cite a ratified RFC in the commit/PR body.
4. **Build order is P0 → P4, ownership is domain-based.** Sequencing follows milestones, not a single serialized list:
   - **P0** — freeze schemas and interfaces; kernel, storage, coordinates, Scene + Observation Graph, Plugin/Capability contracts, error codes, Recipe schema. Work here is parallel *within* each domain, but all P0 contracts must land before integration-heavy work depends on them.
   - **P1** — scheduler + worker protocol + mock adapters + SDK/CLI skeleton (Core Platform + UX), camera/SLAM importer scaffolds.
   - **P2** — Geometry Graph logic, quality/uncertainty channels (Geometry).
   - **P3** — first real adapters behind mocks (Photogrammetry COLMAP/OpenMVS, SLAM KISS-ICP/GTSAM), AI priors through the ADR-006 validation gate (AI).
   - **P4** — recipes end-to-end, adaptive engine, viewer/export, benchmarks.
   - Domains proceed within their lane as fast as dependencies allow; a domain never blocks on another domain's milestone unless it consumes the frozen contract.
5. **Architecture Debt = 0 in the kernel.** No TODO/FIXME/HACK in `core/**`, `engine/**`, `schemas/**`; a task is either built to the architecture or deferred (ADR-031). Experimental code lives only in `python/research/**` and `benchmarks/experimental/**`.
6. **Capabilities over names.** Teams build adapters that declare capabilities; the engine selects by capability. A team never hard-codes another team's adapter.
7. **AI = priors.** AI outputs enter only as priors attached to observations through validation; the classical core validates before anything becomes geometry (ADR-006).

## 3. Dependencies between lanes

```
P0 contracts (all teams, frozen)          ← everything depends on these
   │
   ├── Core Platform: storage/scheduler/workers/IPC
   ├── Geometry:       Scene, Observation Graph, coordinates, calibration
   └── (Photogrammetry/SLAM/AI: adapter + importer scaffolds)
   │
   ├── P1: mock pipeline end-to-end, SDK/CLI
   ├── P2: Geometry Graph + quality channels
   ├── P3: real adapters + AI priors (behind mocks where licensed)
   └── P4: recipes, adaptive engine, viewer, exports, benchmarks
```

The mock pipeline (P1) is the first point where all six teams' outputs integrate: it walks the stage graph and produces placeholder scene versions end-to-end (ADR-031).

## 4. Definition of done per team

A team's milestone is done when its lane's unit/integration/property/fault-injection tests pass, its P0 contracts are frozen and schema-valid, and no changes touch protected surfaces without a cited RFC.

## References

- `docs/architecture/system-overview.md` (§3 team ownership table)
- `docs/development/testing.md`, `docs/development/build.md`
- CONSTITUTION.md, ADR-001, ADR-031, ADR-037
