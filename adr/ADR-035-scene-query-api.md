# ADR-035 — Scene Query API

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Every subsystem reads the Scene: the scheduler feeds it tasks, the quality engine (ADR-030) scores it, the uncertainty engine reasons over it, the workflow (ADR-028) diffs versions, and clients render it. If each consumer reached into Observation Graph, Geometry Graph, and Relationship Graph internals directly, then any internal restructure (new element type in ADR-032, new channel in ADR-025) would ripple across the platform. The Scene is a protected surface, but its read paths are not yet unified, so internal storage is effectively frozen by every current consumer. We need one stable, read-only surface over all three graphs.

## Decision

The Scene exposes a unified read-only Scene Query API, `scene.query()`, whose query objects return typed, read-only views over scene contents: `.images()`, `.lidar()`, `.points()`, `.visibleFrom(camera)`, `.quality()`, `.uncertainty()`, with spatial, frame-scoped, and provenance-filtered variants to follow. Queries cross graph boundaries: `visibleFrom(camera)` joins Observation Graph geometry with Geometry Graph frames, and `.quality()`/`.uncertainty()` surface first-class channels (ADR-025) without exposing storage layout. The API is deliberately read-only: mutation stays behind the Scene's versioned write path (ADR-033), which keeps queries safe to run concurrently and preserves immutability. Because all consumers use the query API, internal storage is replaceable — Observation Graph, Geometry Graph, and Relationship Graph can evolve independently without breaking callers. The API is implemented in M0 as part of the Scene Graph and is a protected surface (returns strict domain types, never raw matrices or primitives).

**Deferred to:** post-M0 — relationship- and uncertainty-specific query variants and spatial-index-accelerated queries; the core query surface ships in M0.

## Alternatives

- **Direct graph access for every consumer:** rejected — couples consumers to storage and makes internal evolution impossible.
- **SQL-over-everything:** rejected — leaks storage into the domain API and is wrong for graph and spatial traversals.
- **RPC-style heavy queries only:** rejected — latency and ergonomics are wrong for interactive desktop and SDK usage.

## Consequences

- Positive: internal structures remain replaceable behind one stable contract; consumers are small and readable; concurrency is safe because the surface is read-only; SDK, CLI, desktop, and remote APIs (ADR-037) can all share the same query semantics.
- Negative: an API layer to design and maintain; some queries are expressively awkward until richer variants land; performance-sensitive paths may want to bypass it — which is not allowed.
- Risks and mitigations: risk of performance regressions — mitigated by index-backed query variants and benchmarks (ADR-029); risk of the API drifting from user needs — mitigated by prototyping all consumers against it in M0; risk of accidental mutation sneaking in — mitigated by constitution protection and read-only types.

## References

- `docs/specifications/scene-model.md`
- `docs/specifications/geometry-model.md`
- ADR-023 (scene as central object), ADR-032 (geometry graph), ADR-025 (provenance/uncertainty channels), ADR-030 (quality engine), ADR-033 (versioning), ADR-037 (public API surfaces)
