# Spatial Platform — Product Blueprint

- **Status:** ratified (P0) — strategic direction
- **Scope:** product architecture and long-term strategy, not a technical spec
- **Governance:** `spatial-rfcs` (ADRs 001–037, CONSTITUTION.md); cross-reference: `spatial-rfcs/CONSTITUTION.md`

This document is the top-level product strategy for the Spatial Platform. It answers the question "what must the system do in 5–10 years", and it outranks all other documentation except the Architecture Constitution for strategic direction. Technical details live in the ADRs and the specification documents; this document explains why those decisions exist.

---

## 1. Product Vision

The Spatial Platform is **a Spatial Computing Platform**, not a "RealityCapture clone". RealityCapture and Metashape are photogrammetry applications that reconstruct a model from a photo set. We are building the layer underneath that workflow and extending it far beyond it: the platform becomes **the core of spatial data** for any organization that captures, reconstructs, measures, or simulates the physical world.

The defining loop of the platform:

- **Any sensor feeds a Scene.** A Scene is the central domain object (ADR-023): a live, versioned, queryable model of a place. Photos, LiDAR, video, IMU, GNSS, RGB-D, panoramic, thermal — every sensor's output enters a Scene as observations.
- **Any consumer reads a Scene.** BIM, CAD, GIS, VFX, digital twins, web viewers, robots, simulators, and analytics all consume scenes through stable, capability-based APIs. They never depend on the pipeline that produced the geometry.

Consequences of this positioning:

1. The platform is **data and processing infrastructure**, not an end-user application. Reconstruction is one family of capabilities; it is neither the product nor the boundary.
2. The platform is **scene-centric**, not pipeline-centric. A pipeline run is a transient thing that writes into a Scene; the Scene outlives every run.
3. The platform is **engine-agnostic**. COLMAP, OpenMVS, GTSAM, KISS-ICP, Open3D, VGGT, and any future engine are replaceable workers behind adapters (ADR-004, ADR-034).
4. The platform wins by **owning the data substrate**: geometry with provenance, uncertainty, versioning, and relationships. Tools are commodities; data with a known provenance is the moat.

The 10-year goal in one sentence: **if a sensor measured it, a Scene should contain it; if software needs reality, it should read a Scene.**

---

## 2. Long-term Architecture

The platform's value chain is a pipeline of stages. Each stage is a set of capabilities with well-defined interfaces, not a single code path:

```
Capture → Scene → Geometry → Knowledge → Digital Twin → Simulation → Automation → AI
```

- **Capture.** Import of any sensor data — images, scans, poses, calibrations, control points — as first-class observations. This stage creates the Scene's Observation Graph and Sensor Graph; it produces no geometry yet.
- **Scene.** The central domain object, assembled by the previous stage: Observation Graph (what was measured, by whom, when), Geometry Graph (GeometryElements and their relationships), and Relationship Graph (links between observations, geometry, and semantic objects). Versioned like a source repository (ADR-033).
- **Geometry.** Reconstruction and geometry production: SfM/MVS, SLAM, LiDAR registration, Gaussian Splatting, TSDF, and every future technique. All produce GeometryElements (Point, Triangle, Voxel, Gaussian, Spline, Primitive, SurfacePatch, ImplicitSurface) — never producer-native formats (ADR-032).
- **Knowledge.** Geometry becomes understood: semantics, instances, materials, quality, uncertainty, control/survey constraints. This is where a mesh becomes "a building with three rooms, surveyed to 2 cm".
- **Digital Twin.** A knowledge-rich Scene evolves over time: temporal epochs (ADR-036), change detection, branching and merging. The Scene becomes a persistent, versioned twin of a real place rather than a one-off snapshot.
- **Simulation.** The twin is usable as a substrate: physics, lighting, line-of-sight, robotics planning, what-if analysis. Simulation consumes the twin; it does not live inside the platform.
- **Automation.** Recipes (ADR-026) and adaptive pipelines (ADR-027) run the capture→twin loop autonomously: schedule, retry, replan, re-run only what changed.
- **AI.** All of the above is powered and augmented by learned models — as **priors only** (ADR-006). AI proposes hypotheses; the classical geometric core validates and commits them as geometry.

The stages are additive over time, not all present on day one. M0 is the engineering skeleton for Capture and Scene; the later stages are the roadmap.

---

## 3. Project Boundaries

The platform **never becomes** CAD, BIM, or GIS — but it must be **the best neighbor they have ever had**.

- **CAD** — parametric, associative modelling for design intent. Out of scope as a product.
- **BIM** — building information modelling: discipline-specific objects, IFC, LOD contracts, construction workflows. Out of scope as a product.
- **GIS** — large-area map management, CRS mosaics, raster/vector web services, feature databases. Out of scope as a product.

For each of these domains the platform exposes **stable import and export APIs**, and nothing more:

- **CAD**: export scene geometry as Parasolid/STEP/DWG-compatible surfaces and solids; import CAD assemblies as Scene geometry with named frames.
- **BIM**: export to IFC (building geometry, rooms, element classes); import IFC as Semantics + Primitive geometry so scans and models can be compared in one Scene.
- **GIS**: export georeferenced point clouds, meshes, and rasters to GeoPackage / cloud-tile formats with a coordinate reference system; import survey control and ground-truth rasters.

The rule: **the platform moves spatial data between the physical world and every professional ecosystem; it never competes with those ecosystems.** When a customer asks "why isn't this a CAD program?", the answer is that the platform is the reality layer, and their CAD/BIM/GIS tools are the design layer on top.

---

## 4. Roadmap

A milestone ladder. Every milestone has a rough goal and an **exit criterion**: an observable, testable statement of what must be true before the next milestone starts. Milestones are not releases; several may be in flight at once.

- **M0 — Foundation.** Engineering skeleton: Project Core (`.spx`), the SHA-256 artifact store, coordinate system, Scene + Observation Graph, Scheduler + Worker protocol, SDK/CLI, and mock adapters. *Exit: a trivial photo pair round-trips through import → observation graph → mock worker → artifacts, with provenance, under the constitution checks.*
- **Photogrammetry.** Real COLMAP/OpenMVS adapters behind capabilities: sparse/dense reconstruction, meshing, texturing into a Scene. *Exit: parity with reference photogrammetry tools on a standard benchmark set, measured by the benchmark framework (ADR-029).*
- **Hybrid.** Multi-sensor fusion: LiDAR + photo, GNSS/control points, depth sensors in a single Scene with joint optimization. *Exit: survey-grade scene with mixed inputs; georeferencing passes QA.*
- **SLAM.** Live and incremental reconstruction: video streams, odometry, loop closure via GTSAM factor-graph backends (ADR-005) and KISS-ICP. *Exit: a handheld capture produces a drift-corrected, explorable Scene in one session.*
- **Gaussian.** Gaussian Splatting (gsplat) adapters: real-time rendering-quality appearance in the Scene, hybrid mesh/gaussian LoDs. *Exit: Gaussian representation selectable as a first-class LoD with export.*
- **Realtime.** Interactive feedback: progressive reconstruction while capturing, on-device preview, low-latency scene updates. *Exit: pipeline latency from frame to scene update in the tens of milliseconds; capture-and-review workflows are interactive.*
- **Cloud.** Distributed processing: cloud workers, resumable jobs, shared projects, browser viewing. *Exit: a cloud reconstruction run scales past the largest single machine the platform can address.*
- **Digital Twin.** Temporal epochs, branching/merging, change detection, relationship graph depth (ADR-036). *Exit: a Scene answers "what changed since last month" as a first-class query.*
- **Enterprise.** Governance: authN/authZ, SSO, audit logs, role-based project sharing, compliance, multi-tenant cloud. *Exit: a named enterprise customer runs production without an account team on call.*
- **SDK.** Stable plugin and extension APIs: third-party capabilities, custom sensors, custom importers/exporters, embedded deployments. *Exit: an external team ships a plugin through the Plugin API without contacting core engineers.*
- **Marketplace.** Distribution: a registry of verified plugins, adapters, and recipes; commercial and open-source tiers. *Exit: a third-party vendor sells a capability through the marketplace.*

---

## 5. Why RealityCapture is Limited

An engineering analysis, not a criticism. RealityCapture is a superb **pipeline-centric, reconstruction-only** product: it takes a photo set and produces a textured mesh, fast. The Spatial Platform is **scene-centric** and is a **spatial platform**. That difference has hard engineering consequences — things RealityCapture cannot natively do because of its design, not its effort:

1. **It cannot answer "what observations created this point".** Reconstruction products consume images into an internal optimization and then throw the image-level linkage away. There is no persistent observation→point correspondence to query.
2. **Per-point provenance and uncertainty are not first-class data.** Its export pipeline can write per-vertex confidence as a side channel, but the data model does not carry, persist, or reason over it. The platform stores provenance channels and uncertainty as first-class scene data (ADR-025, Constitution Principle 14).
3. **Scenes cannot be versioned like git.** A reconstruction is a state, not a lineage. The platform treats every mutation as a new immutable scene version with a parent chain (ADR-033), so snapshots, branching, and reverts are structural, not backup conventions.
4. **It cannot serve as a persistent digital twin substrate.** Because there is no Scene with temporal epochs, relationships, and semantics, it cannot be the live model that a BIM, a robot, or a simulator reads over years. It is a generator of deliverables, not a host of reality.
5. **It is a single pipeline, not a platform.** Adding a new technique (SLAM, LiDAR fusion, Gaussian Splatting, a future AI engine) means rebuilding the product around it. The platform adds a capability behind an adapter, and the Scene grows.

The strategic conclusion: a pipeline product and a spatial platform converge today but diverge at the timeline. Every year, reconstruction quality gets commoditized by new engines and AI priors; the durable asset is the **Scene**: its provenance, its uncertainty, its versioning, its relationships. That is what we build and what they cannot.

---

## 6. Architectural Constitution

The following surfaces **can never break**. They are frozen by `spatial-rfcs/CONSTITUTION.md`; changing any of them requires a ratified RFC and Architecture Review. This section is the product-level summary of that document — read the Constitution itself for the normative text.

- **Coordinate System** — conventions, units, strict typed transforms (`WorldFromX`, `RigFromSensor`). A transform without named frames is invalid (ADR-007, ADR-018).
- **Scene** — the central domain object and its three graphs: Observation, Geometry, Relationship (ADR-023, ADR-024, ADR-032).
- **GeometryElement** — the geometry abstraction: Point, Triangle, Voxel, Gaussian, Spline, Primitive, SurfacePatch, ImplicitSurface (ADR-032). No producer forces its native representation into Core.
- **Observation** — the immutable measurement hierarchy and the Observation Graph (ADR-024). Facts are never mutated; corrections create new observations.
- **Artifact** — the SHA-256 content-addressed store and its manifest/provenance format (ADR-010). Artifacts are immutable and deduplicated.
- **UUID** — the canonical identifier format and assignment rules for every entity.
- **Plugin API** — Plugin, PluginManager, Capability, and Adapter interfaces. Plugins **never bypass Core** (Constitution Principle 9).
- **Capability API** — the capability taxonomy and negotiation. Capabilities are preferred over implementation names (Principle 8): components declare what they can do, and the engine selects implementations by capability.

Two supporting rules from the Constitution that protect these surfaces in practice: no large binary data in SQLite (payloads live in the artifact store), and AI outputs appear in a Scene **only as priors**, never as authoritative geometry (ADR-006).

---

## 7. What May Change

Everything not in Section 6 may change — and much of it **should** change, routinely. The replaceable layer is the entire algorithm surface:

- **Backends are swappable by design.** COLMAP → OpenMVG → VGGT, or any future engine, is a change to an adapter, never to Core, Scene, or the Artifact Store (Constitution Principle 15, ADR-004). The Constitution explicitly lists backend choice and version as changeable **without an RFC** (§3).
- **Versions change continuously.** Engine versions, dependency versions, schema versions (via migration) all bump as a normal part of development. The `.spx` schema is versioned and migrates independently (ADR-009).
- **Capabilities evolve.** The capability taxonomy grows as new adapters and techniques land; that is the platform getting richer, not breaking.

Why is replacement the normal case and not an exceptional one? Because **geometry quality is a moving target**. The best SfM/MVS/SLAM/AI system changes every 6–18 months. A platform that can adopt the best engine of the moment — and keep the Scene, provenance, and workflow intact — turns churn into a competitive advantage. A platform that hard-wires an engine dies with it. Everything below the capability layer stays; everything above it is a ride you can change without stopping.

---

## 8. North Star

In 10 years the platform must be able to:

1. **Understand a scene** — geometry, semantics, materials, quality, and change over time, all in one queryable model.
2. **Build a digital twin** — a persistent, versioned, epoch-aware Scene that survives for the life of the asset it represents.
3. **Automatically choose the best algorithms** — for the given sensor mix, data quality, and requested product (ADR-027), no user picking engines.
4. **Support any sensors** — cameras, LiDAR, RGB-D, GNSS, IMU, panoramic, thermal, and sensors not yet invented.
5. **Work locally and in the cloud** — the same Scene model, from a laptop to a distributed cluster.
6. **Be a platform, not an application** — third parties build products on top of it through stable APIs.

**North star metrics** (measured by the benchmark framework, ADR-029):

- **Accuracy and completeness parity with the best commercial tool** on ETH3D and Tanks-and-Temples, within a stated tolerance band, at every photogrammetry milestone.
- **Time-to-model**: median wall-clock from raw capture to a validated scene product, reduced by an order of magnitude over a reference commercial pipeline by the realtime/cloud milestones.
- **Provenance coverage**: 100% of geometry elements and artifacts carry resolvable provenance and uncertainty; zero unreproducible pipeline runs.
- **Plug-in count**: the number of third-party capabilities in the marketplace (target: a double-digit count of independently developed, shipping plugins by the SDK/marketplace milestones).
- **Fidelity of the data substrate**: fraction of scene features (provenance, uncertainty, versioning) that are preserved losslessly through import→export to every supported external format.
- **Adoption**: a named set of production deployments across photogrammetry, surveying, construction, VFX, and robotics, each consuming scenes through the public APIs.

The North Star is not "best photogrammetry tool". It is: **the platform where reality lives as data — measured once, trusted forever, understood completely.**
