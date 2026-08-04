# ADR-033 — Immutable Scene Versioning

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Reconstruction proceeds through distinct stages — import, align, dense, mesh, texture — and users routinely want to undo, compare, branch experiments, and cite exactly which scene state produced a deliverable. The Artifact Store (ADR-010) is content-addressed and immutable, which makes cheap snapshots possible: scene payloads are CAS SHA-256 blobs, so two versions that share geometry share storage automatically. The workflow (ADR-028) needs versioning as its undo/comparison primitive, and the provenance rules (ADR-012, ADR-025) need every scene state to record its own creation history. Today a Scene has no version concept.

## Decision

Scenes are versioned git-style: every stage produces a new immutable Scene version, e.g. `v1 → import → v2 → align → v3 → dense → v4`. Each version records `version_id`, `parent`, `stage`, and `created_by`, forming a version chain. Undo is pointer movement along the chain; compare is a diff between two version snapshots through the Scene Query API (ADR-035); branching enables experiments that later merge. Because payloads are CAS-addressed, snapshotting is cheap and deduplicated; the metadata for each version is a small record in the SQLite project database (ADR-009). The active version is an explicit pointer, not an implicit state. In M0 only the version-chain metadata (`version_id`, `parent`, `stage`, `created_by`) ships, so every M0 operation that mutates a scene records a new version; full copy-on-write scene semantics, branching, and merge ship later.

**Deferred to:** post-M0 — full copy-on-write semantics, branching/merge, and version-aware diff; M0 ships the version-chain metadata only.

## Alternatives

- **Mutable scenes with explicit save:** rejected — impossible to guarantee reproducibility or undo, and defeats the immutability principle.
- **Full deep-copy per version:** rejected — storage and IO explosion; CAS dedup exists precisely to avoid this.
- **Versioning only at the artifact level:** rejected — users think in scenes; artifact versioning alone cannot express "the scene at stage X".

## Consequences

- Positive: undo, compare, and branching become data operations; provenance per version is unambiguous; reproducibility across experiments is exact; workflows (ADR-028) and digital twin monitoring (ADR-036) build on the same mechanism.
- Negative: version-chain bookkeeping; some operations become "create new version" by design, which can surprise; merge semantics are genuinely hard and deferred.
- Risks and mitigations: risk of unbounded version retention — mitigated by CAS GC and retention policy (ADR-010); risk of confusion between artifact versions and scene versions — mitigated by explicit naming and the `version_id`/`parent` chain; risk of merge complexity — mitigated by deferring merge until the full semantics are ratified.

## References

- `docs/specifications/scene-model.md`
- `docs/specifications/artifact-format.md`
- ADR-008/ADR-009/ADR-010 (project storage, metadata separation, artifact store), ADR-028 (workflow), ADR-031 (M0 scope), ADR-035 (scene query API), ADR-036 (digital twin epochs)
