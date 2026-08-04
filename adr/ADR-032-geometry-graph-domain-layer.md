# ADR-032 — Geometry Graph as Independent Domain Layer

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Reconstruction products historically model the world as either a point cloud or a mesh. That is wrong for this platform: the same scene must be represented as sparse points, dense clouds, meshes, voxel grids, gaussian splats, splines, primitives, surface patches, and implicit surfaces — often simultaneously (gaussian splat plus textured mesh from the same data). Each geometry producer (COLMAP, LiDAR, Gaussian Splatting, NeRF, TSDF, CAD/BIM, manual, generative) has a native representation. Principle 2 (geometry is algorithm-independent) and Principle 13 (every element has an explicit frame) require a representation-neutral domain layer. The Observation Graph (ADR-024) is the canonical measurement substrate; geometry is derived, must not live inside it, and must not be forced into mesh/point-cloud terms.

## Decision

The Scene is composed of three graphs: Observation Graph + Geometry Graph + Relationship Graph. The Geometry Graph is an independent domain layer whose nodes are `GeometryElement` instances: a `GeometryElement` base type with concrete `Point`, `Triangle`, `Voxel`, `Gaussian`, `Spline`, `Primitive`, `SurfacePatch`, and `ImplicitSurface` types. Every geometry producer feeds `GeometryElement`s; no producer may force its native representation into Core. Every element carries an explicit coordinate frame, provenance (ADR-025), and quality channels. The Geometry Graph stores containment and spatial organization (scene-relative placement, element hierarchies, grouping) while leaving measurement semantics to the Observation Graph; the Relationship Graph later binds observations to geometry (e.g. which gaussians project to which images) for the Uncertainty and Quality Engines. In M0 the Scene Graph holds only the minimal `GeometryElement` base and placeholder concrete types sufficient for the Scene Query API; full Geometry Graph topology and traversal logic ship later.

**Deferred to:** post-M0 — full Geometry Graph logic (containment, spatial indexing, topology, cross-element queries); M0 keeps base types and query scaffolding.

## Alternatives

- **Mesh as the core abstraction:** rejected — cannot represent gaussians, voxels, splines, or implicit surfaces; forces lossy conversion for every producer.
- **Point cloud as the core abstraction:** rejected — fails for parametric and surface-patch producers and loses topology.
- **Producer-native storage in Core:** rejected — violates Principle 2 and couples Core to every backend forever.

## Consequences

- Positive: producers emit their native element type and Core stays representation-neutral; a scene can mix gaussian splats and meshes; element types are extensible without touching Observation Graph; queries are uniform via the Scene Query API.
- Negative: abstraction cost for simple consumers; concrete type set must be curated to avoid unbounded growth; some producers need conversion shims into an element type.
- Risks and mitigations: risk of element-type explosion — mitigated by the P0 schema governing `GeometryElement` subtyping; risk of producers bypassing elements — mitigated by constitution protection of `core/scene/geometry/**`; risk of anemic base class — mitigated by M0 placeholder types plus the ratified element schema.

## References

- `docs/specifications/geometry-model.md`
- `docs/specifications/scene-model.md`
- ADR-023 (scene is the central domain object), ADR-024 (observation graph), ADR-025 (per-point provenance), ADR-031 (M0 scope), ADR-035 (scene query API)
