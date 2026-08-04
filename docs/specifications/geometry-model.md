# Geometry Model Specification

- **Status:** ratified (P0)
- **References:** ADR-032, ADR-007, ADR-010, ADR-018, ADR-025, ADR-033, ADR-006
- **Protected surface:** `core/scene/geometry/**` (Constitution §2)

## 1. Purpose

Defines the **Geometry-first** model of the Spatial Platform. Geometry is stored as **Geometry Elements** — not as mesh/point-cloud/splat-specific structures. Any producer (COLMAP, OpenMVS, LiDAR, Gaussian Splatting, NeRF, TSDF, CAD/BIM, manual modelling, generative models) contributes geometry by emitting `GeometryElement`s through an adapter; the Core never depends on any producer's native representation.

This specification is normative for `core/scene/geometry/**`.

## 2. The GeometryElement Abstraction

```cpp
class GeometryElement {
    Uuid element_id;                    // stable identity, immutable
    GeometryKind kind;                  // Point | Triangle | Voxel | Gaussian |
                                        // Spline | Primitive | SurfacePatch | ImplicitSurface
    CoordinateFrameRef frame;           // EXPLICIT frame (Architecture Principle 13)
    DistanceMeters? bounds;             // optional spatial bounds (AABB)
    LoDLevel lod;                       // level of detail
    ElementRef parent;                  // optional parent in the Geometry Graph
    ProvenanceRef provenance;           // who produced this, from what, with which config
    UncertaintyRef? uncertainty;        // optional uncertainty block (ADR-025)
    ArtifactRef payload;                // the actual data, in the artifact store
};
```

Rules:

1. **Explicit frame.** Every element must name its coordinate frame. Elements without a frame are rejected at the boundary.
2. **Payloads are artifacts.** Geometry data buffers are immutable, content-addressed artifacts (ADR-010). The `GeometryElement` is a typed handle to them.
3. **Provenance is never discarded** (Principle 12). An element always records its producer, inputs, and configuration.
4. **No raw matrices** in element interfaces (ADR-007, ADR-018): transforms are `WorldFromX`, `XFromY`, frames are named; positions are `DistanceMeters`-typed.

## 3. Concrete Element Kinds

| Kind | Data | Notes |
|---|---|---|
| **Point** | positions[], optional channels: normals[], colors[], intensity[], confidence[] | ordered or unstructured point set; canonical channel layout in `schemas/json/geometry-element.schema.json` |
| **Triangle** | indexed triangles + per-vertex attributes | triangle soup or indexed mesh; topology may be absent (deferred) |
| **Voxel** | grid metadata + density/occupancy/color channels | sparse or dense grid; TSDF representation as a Voxel subtype |
| **Gaussian** | per-splat: mean, covariance/SH params, opacity, color | conforms to gsplat-compatible export later |
| **Spline** | curve params, degree, control points | reserved for survey/vector geometry |
| **Primitive** | parametric: plane/cylinder/sphere/box + params | reserved for CAD-like and BIM geometry |
| **SurfacePatch** | trimmed parametric surface, boundary | reserved for NURBS/interpolated surfaces |
| **ImplicitSurface** | field grid or neural field reference | reserved for NeRF/neural surfaces; holds a field artifact, never baked density in Core |

M0 ships **Point, Triangle, Voxel, Gaussian** as placeholder concrete types (structure + serialization, no production logic). Spline, Primitive, SurfacePatch, ImplicitSurface are defined in schema but not instantiable until their milestones.

## 4. Geometry Graph

- Elements form a **DAG** (not a tree): containment (`parent`), registration (two elements in different frames tied by a transform), correspondence (element ↔ observation), and LoD edges.
- A **mesh** is not a primitive type — it is a composition of Triangle elements plus a topology/level structure. A **point cloud** is a Point element (or a chunk hierarchy of them).
- **Relationship Graph** (scene level) links geometry ↔ observations ↔ semantic objects (see `scene-model.md` §3).

## 5. Levels of Detail (LoD)

- Elements carry an LoD level; edges connect LoD representations (e.g. sparse points → dense cloud → mesh → textured mesh).
- LoD selection is a query concern (`scene.query().geometry().at(LoD::Mesh)`), not a storage concern.

## 6. Uncertainty and Per-Point Data (ADR-025)

- Element-level `UncertaintyRef` points to an optional uncertainty artifact.
- Per-point/per-vertex channels (in the artifact payload): `covariance`, `confidence`, `source_count`, `residual`, `normal_confidence`, `texture_confidence`, `color_confidence`, and **provenance channels** (contributing observation UUIDs, contributing artifacts, BA iteration).
- Storage uses compact references + a global contribution table to bound size. Channels are optional; the schema fixes them now, population is deferred to post-M0 (ADR-025).
- Uncertainty is **first-class data** (Principle 14), never recomputed-only.

## 7. Producers and Replaceability (Principle 15)

Every backend maps onto element kinds via its adapter:

```
COLMAP sparse  → Point elements (+ ImageObservation correspondences)
OpenMVS mesh   → Triangle elements (+ SurfacePatch reserve)
LiDAR/KISS-ICP → Point elements (intensity channel)
Gaussian 3DGS  → Gaussian elements
NeRF/VGGT      → ImplicitSurface / Point elements (as validated priors)
TSDF           → Voxel elements
CAD/BIM        → Primitive + SurfacePatch elements
Manual/generative → any registered kind via Plugin API
```

Replacing COLMAP with OpenMVG or VGGT changes only an adapter, never Core (ADR-004, ADR-034).

## 8. Serialization

- Buffers: artifact store, per-kind binary layout defined in `docs/formats/` + `schemas/json/geometry-element.schema.json`.
- Wrapper: protobuf `GeometryElement` (metadata) + typed payload reference.
- Frames must be resolvable within the Scene frame graph at serialization time; unresolved frames fail validation.

## 9. Invariants

1. `element_id` immutable; content changes produce a new element (new id) or new artifact hash (id stable, payload versioned).
2. Every element has a resolvable `coordinate_frame`.
3. Geometry payloads are immutable artifacts with SHA-256 identity.
4. AI outputs cannot be written directly as elements; they must pass the validation gate and enter as observations/priors first (ADR-006).
5. LoD edges and parent edges form a DAG — cycles are a validation error.
