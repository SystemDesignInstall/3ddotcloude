# Spatial Platform — System Overview

- **Scope:** technical system overview of the monorepo `spatial-platform`
- **Governance:** `spatial-rfcs` (ADRs 001–037, CONSTITUTION.md)
- **Protective spec:** `docs/specifications/scene-model.md`, `docs/specifications/geometry-model.md`

## 1. Layered architecture

```
                    ┌────────────────────────────────────────────────┐
                    │               PLATFORM SURFACES                │
                    │   CLI    Python SDK    C++ SDK    UI    API   │
                    └───────────────────────┬────────────────────────┘
                                            │  the .spx contract (protobuf/JSON/SQL)
                    ┌───────────────────────▼────────────────────────┐
                    │              libspatial_core (C++20)           │
                    │  ┌──────────────────────────────────────────┐  │
                    │  │  core/  Scene, Observation Graph,        │  │
                    │  │  GeometryElement, coordinates, artifacts │  │
                    │  ├──────────────────────────────────────────┤  │
                    │  │  engine/ pipeline DAG, scheduler,        │  │
                    │  │  recipes, quality, capability registry   │  │
                    │  └──────────────────────────────────────────┘  │
                    └───────────────────────┬────────────────────────┘
                                            │  adapter/plugin interfaces
                                            │  (Capability API, Plugin API)
                    ┌───────────────────────▼────────────────────────┐
                    │        ADAPTERS + PLUGINS (isolated)           │
                    │  importer/exporters · custom capabilities      │
                    └───────────────────────┬────────────────────────┘
                                            │  Worker protocol (IPC, ADR-011/012)
                    ┌───────────────────────▼────────────────────────┐
                    │        ISOLATED WORKER PROCESSES               │
                    │  COLMAP  OpenMVS  GTSAM  Open3D  KISS-ICP     │
                    │  VGGT    gsplat   Ceres   (any future engine) │
                    └────────────────────────────────────────────────┘
```

Layering rules (Constitution): surfaces talk to Core through the `.spx` contract; plugins reach the system only through Plugin → Adapter → Capability interfaces and **never bypass Core**; no geometry producer forces its native representation into Core.

## 2. Data flow

```
Import ──► Artifact ──► Scene graphs ──► Pipeline DAG ──► Scheduler ──► Workers
 (any     (SHA-256    (Observation +   (recipe +        (capability    (COLMAP,
 sensor    CAS,       Geometry +        capability       selection,     OpenMVS,
 data)     ADR-010)   Relationship,     resolution,      cache,         VGGT, …)
                      ADR-024/032)      ADR-026)         durability)    
                                                                │
                          ┌─────────────────────────────────────┘
                          ▼
                  Artifacts with provenance ──► Scene (versioned, ADR-033)
```

1. **Import** — any sensor data enters as observations through an importer adapter.
2. **Artifact** — payloads are written once, content-addressed (SHA-256 CAS); metadata goes to SQLite (ADR-009). No large binaries in the database.
3. **Scene graphs** — observations populate the Observation Graph; later stages grow the Geometry and Relationship Graphs.
4. **Pipeline DAG** — a recipe (ADR-026) is resolved into a DAG of steps, each declared as capabilities.
5. **Scheduler** — picks implementations by capability, honors cache (ADR-020), persists and replays runs (ADR-028).
6. **Workers** — isolated processes (ADR-011) running COLMAP, OpenMVS, GTSAM, Open3D, KISS-ICP, VGGT, gsplat, and any future engine over a stable IPC protocol (ADR-012).
7. **Artifacts with provenance** — each result records producer, inputs, config, and commit; results commit into a new immutable Scene version.

AI model outputs (VGGT, learned depth/masks) enter **only as priors attached to observations** (ADR-006); the classical core validates before anything becomes geometry.

## 3. Domain team ownership

| Team | Owns | Constitution-protected paths |
|---|---|---|
| **Core Platform** | `.spx` project format, artifact store, coordinates, Scene/Observation Graph, scheduler, IPC, SDK/CLI | `core/scene/**`, `core/artifacts/**`, `core/coordinates/**`, `schemas/**` |
| **Geometry** | GeometryElement kinds, Geometry Graph, LoD, meshing/decimation surface, provenance channels | `core/scene/geometry/**` |
| **Photogrammetry** | COLMAP/OpenMVS/VGGT adapters, sparse/dense/mesh/texture capability workflows, recipes | `adapters/`, `engine/` (capability-level) |
| **SLAM** | Live/online reconstruction, GTSAM factor graphs, KISS-ICP, rig calibration, temporal graph | `core/scene/observation_graph/**`, `adapters/` |
| **AI** | Learned priors (depth, poses, semantics, gaussians), validation gate, uncertainty integration | research modules; priors only, never authoritative geometry |
| **UX** | CLI ergonomics, UI, browser/cloud viewing, capture and review workflows | platform surfaces |

## 4. M0 scope (ADR-031)

Engineering skeleton only, built to the architecture: Project Core (`.spx`), Artifact Store, coordinates, Scene + Observation Graph, Scheduler + Worker protocol, SDK/CLI, and mock adapters for interface isolation (ADR-021). Production geometry logic is deferred; the structure is normative.
