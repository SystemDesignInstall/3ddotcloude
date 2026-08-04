# Spatial Platform — Product Vision

**One scene. One coordinate graph. Many sources. Many consistent representations.**

The Spatial Platform turns physical reality into a first-class, persistent, queryable data asset — a **Scene** — that any tool, discipline, or machine can read and extend.

## Who it is for

- **Photogrammetrists and surveying firms** — surveyed-accurate reconstructions with georeferencing and control points they can defend in front of a client.
- **Construction and engineering** — scan-to-BIM comparison, progress tracking, and a reality layer that stays aligned with their models over the life of a project.
- **VFX and media** — photorealistic meshes, gaussians, and footage-scale capture with provenance they can trust in a shot pipeline.
- **Digital twin and robotics teams** — a living, versioned twin of a real place that changes are recorded against, not a one-off snapshot that dies with the export.

## What a Scene is

A Scene is the platform's core asset. It is not a file and not a reconstruction run; it is a versioned, queryable model of a place that accumulates over time:

- **Observation Graph** — what was measured, by which sensor, at what time. Photos, LiDAR, RGB-D, GNSS, IMU, panoramic — everything is a first-class, immutable observation (ADR-024).
- **Geometry Graph** — the same reality as sparse points, dense cloud, mesh, Gaussian splat, voxel field, or implicit surface: representations of one Scene, never incompatible exports (ADR-032).
- **Relationship Graph** — how observations, geometry, semantics, and survey control connect to each other.

Every scene version is linked to its parents, like a source repository (ADR-033). Any pipeline run writes *into* a Scene; the Scene outlives every run.

## The core promise

1. **One scene.** Every sensor feeds the same Scene. Nothing lives in a silo, nothing exists only inside a pipeline.
2. **One coordinate graph.** Every geometry element knows exactly which frame it lives in and how it relates to every other frame. No implicit units, no implicit transforms (ADR-007).
3. **Many sources.** Photo sets, laser scans, video streams, survey data, CAD/BIM imports, and AI priors coexist in one model.
4. **Many consistent representations.** The best representation of the moment — mesh, gaussian, implicit — is always available, and swapping it never invalidates the underlying Scene.

## The three-tier value

**Tier 1 — Metric engineering truth.** The platform keeps per-point provenance and uncertainty as first-class data (ADR-025): every result knows what created it, from which observations, with which configuration. When a customer asks "why is this point here and how confident are we?", the Scene answers — from its own data, not a side channel.

**Tier 2 — Visual photorealism.** When appearance matters — VFX, marketing, visualization — the Scene delivers the best current rendering representation, upgraded as techniques evolve (mesh → gaussian → implicit) without redoing the underlying reconstruction.

**Tier 3 — Platform extensibility.** The platform is open at its seams: capabilities over names, plugins that never bypass Core, stable SDKs, and replaceable engines (COLMAP today, VGGT tomorrow). Third parties build products *on* the platform, and the platform grows richer the more the ecosystem does.

## How the workflow goes

A crew captures a site with cameras and a LiDAR scanner. The platform ingests everything into one Scene: photo observations, scan observations, GNSS control. It reconstructs — picking the best algorithms for the data — into geometry the survey team can validate against control points. The client's CAD office imports the Scene as surfaces; the GIS office exports it georeferenced; the digital twin team subscribes to it and sees next month's progress scan as a diff, not a new project.

## Why this wins

Reconstruction tools sell you an output. We sell you the **model of reality** underneath the output — measured once, versioned like source code, trusted forever, and readable by anything: BIM, CAD, GIS, VFX, web viewers, simulators, and robots. The tool gets replaced; the Scene endures.
