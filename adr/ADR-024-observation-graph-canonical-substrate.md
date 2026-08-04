# ADR-024 — Observation Graph as Canonical Measurement Substrate

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Every algorithm in Spatial Platform consumes measurements: images, LiDAR sweeps, IMU samples, GNSS fixes, depth maps, panoramas. Historically each algorithm family (SfM, dense, SLAM, texturing) invents its own measurement access pattern, which produces duplicated ingestion, divergent indexing, and no shared ground truth for validation. The platform needs one canonical, immutable substrate that all algorithms read from, one query API for it, and a home for the GTSAM factors that drive the classical core. The first architecture principles say Observations are immutable measurements and the Observation Graph is the canonical substrate that GTSAM factors operate on, where graph = data and factors = algorithm.

## Decision

Observations are first-class, immutable nodes in the Observation Graph, the canonical measurement substrate owned by the Scene (ADR-023). Each observation is a graph node linking three things: a `Frame`, a `Sensor`, and the `GeometryElement` it observes. The base type is `Observation` with subtypes `Image`, `LiDAR`, `IMU`, `GNSS`, `Depth`, and `Panoramic`, all declared in `core/scene/observation_graph/**`. The Observation Graph exists before any algorithm runs: ingestion builds the graph, and algorithms only ever read it. GTSAM factors are built on top of the graph — the graph is the data, factors are the algorithm — so factor construction references observation nodes and is recomputed as needed without mutating them. A Scene Query API (`core/scene/**` query surface) provides typed, frame-aware access (e.g. all image observations of a frame, all observations seen by a sensor), returning only strict types (ADR-018). Observations are immutable: a corrected observation is a new node with provenance linking to the corrected one (ADR-015), never an in-place edit. Serialization: observations serialize into the project database and artifact manifests in a canonical, versioned schema so the graph round-trips identically. Validation rejects observations that violate the coordinate conventions (e.g. an image with no calibration, a LiDAR point outside its frame) at ingestion time with stable codes (ADR-014).

## Alternatives

- **Per-algorithm measurement access (arrays of "tracks" etc.):** rejected — duplicates ingestion, breaks shared provenance, and makes validation impossible in one place.
- **Observations as plain data rows without graph structure:** rejected — loses the Frame x Sensor x Geometry linkage that factors and queries require.
- **Mutable observations (optimization writes back):** rejected — violates immutability and makes provenance and reproducibility impossible to guarantee.

## Consequences

- Positive: one canonical substrate for all algorithms; GTSAM factor code is clean because it reads a stable graph API; observations are provenance-addressable and auditable; immutable nodes make cache keys (ADR-020) trivially stable; the query API is the single funnel for frame-aware, strictly-typed access.
- Negative: the graph is more structure than a flat table; immutable correction requires the copy-with-provenance pattern; the query API must be designed well up front since it is constitution-protected.
- Risks and mitigations: risk of the graph becoming a bottleneck for large captures — mitigated by lazy/streaming queries for bulk image data; risk of factor code mutating the graph — mitigated by immutable nodes and review; risk of schema drift between serialized and in-memory forms — mitigated by round-trip property tests (ADR-016).

## References

- `docs/specifications/scene-model.md`
- `docs/specifications/geometry-model.md`
- ADR-023 (scene as central domain object — the graph lives in the Scene)
- ADR-005 (GTSAM factors operate on the observation graph)
- ADR-035 (scene query API)
- ADR-018 (strict types — frame-aware query surface)
