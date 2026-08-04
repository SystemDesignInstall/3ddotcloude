# ADR-010 — Content-addressed artifact store

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Artifacts (images, depth maps, point clouds, meshes, descriptors) are large, immutable, frequently deduplicated, and must be verifiable. Name-based addressing collides on updates and obscures what content actually changed. Cache keys must be stable and content-derived.

## Decision

Artifacts are content-addressed by SHA-256. Layout: artifacts/cas/<hash[0:2]>/<hash> (two-character shard prefix). A UUID manifest at artifacts/<uuid>/manifest.json records the artifact's UUID, content hash, producer, input artifact hashes, configuration hash, git commit, and timestamps, forming a provenance graph. Writes are atomic: write to a temp file in the same directory, then rename into place. Artifacts are immutable once stored; identical content deduplicates to a single CAS entry. A garbage collection API removes unreferenced artifacts, and integrity validation verifies hashes on read and during GC.

## Alternatives

- Name-addressed store: rejected. Silent collisions, no deduplication, no integrity verification.
- In-SQLite storage: rejected per ADR-009.
- Recomputed-everything (no cache): rejected. Loses cached task results and provenance.

## Consequences

- Positive: deduplication across tasks and projects; integrity verification detects corruption; immutability gives reliable cache keys (input artifact hashes + configuration hash + producer version + git commit); the provenance graph supports auditing.
- Negative: GC complexity (reference tracking across manifests and the scheduler cache); shard directories grow; hash-computation cost for large payloads.
- Risks and mitigations: GC API with integrity revalidation; fault-injection tests (kill during write) verify the temp-plus-rename atomicity; deduplication counters and GC behavior covered by unit and integration tests.

## References

- docs/specifications/artifact-format.md
- ADR-008 (Project storage format)
- ADR-009 (SQLite metadata separation)
