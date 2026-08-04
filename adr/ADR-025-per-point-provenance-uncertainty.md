# ADR-025 — Per-Point Provenance and Uncertainty as First-Class Data

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Principle 12 (provenance is never discarded) and Principle 14 (quality metadata is first-class) require that every geometry element carry the chain of inputs and the uncertainty that produced it. Dense clouds, gaussian splats, and textured meshes can contain millions of points; a naive per-point expansion of contributing observation UUIDs would grow metadata faster than payload bytes and would bloat the SQLite metadata store (ADR-009). Geometry producers (COLMAP, LiDAR, Gaussian Splatting, NeRF, TSDF, CAD/BIM, manual, generative) must each be able to record provenance and uncertainty without forcing a shared representation into Core. Covariance follows the translation-then-rotation convention and the strict domain types defined in ADR-007.

## Decision

Every GeometryElement may carry optional per-point channels as first-class data alongside geometry and frame. Supported channels: contributing observation UUIDs, contributing artifact UUIDs, the bundle-adjustment iteration that produced the point, per-point residual, confidence, covariance (translation-then-rotation order), source count, and per-channel confidence for normal, texture, and color. Channels are compact typed arrays (fixed-size float arrays, integer index arrays) that reference rows in a global contribution table rather than embedding UUIDs per point. All channels are optional; an element that does not record a channel omits it. The channel schema, contribution-table layout, and encoding are ratified now and fixed in P0 under `schemas/**`; implementation ships later.

**Deferred to:** post-M0 (Photogrammetry milestone) — implementation of channels, the contribution table, and channel writing during reconstruction.

## Alternatives

- **Denormalized per-point UUID lists:** rejected — duplicates observation metadata for every point, bloats manifests and SQLite, and undermines dedup in the CAS artifact store (ADR-010).
- **Recomputed on demand:** rejected — violates Principle 14 (quality metadata is data, not derivation) and makes residuals and covariance unreproducible without replaying the producer.
- **Single per-element scalar confidence:** rejected — too coarse to drive downstream decisions such as viewing, decimation, meshing, and texture selection.

## Consequences

- Positive: provenance and uncertainty survive pipeline transitions; elements can be ranked by confidence; the Quality Engine (ADR-030) and Uncertainty Engine consume channels without recomputation; producers fill only the channels they can compute.
- Negative: schema and encoding complexity; metadata still grows with point count even in compact form; consumers must handle absent channels gracefully.
- Risks and mitigations: risk of metadata growth in SQLite — mitigated by compact arrays, the global contribution table, and keeping payloads out of SQLite (ADR-009); risk of inconsistent covariance conventions — mitigated by strict domain types and the ADR-007 convention; risk of drift between producers — mitigated by schema validation at the adapter boundary.

## References

- `docs/specifications/geometry-model.md`
- `docs/specifications/scene-model.md`
- `docs/specifications/artifact-format.md`
- ADR-007 (coordinate conventions), ADR-009 (metadata/artifact separation), ADR-014 (quality metadata as data), ADR-032 (Geometry Graph)
