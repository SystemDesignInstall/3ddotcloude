# ADR-009 — SQLite metadata separation

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Metadata (scene graph, observation indices, task state, provenance) is small, structured, and query-heavy. Payloads (images, depth maps, point clouds, meshes) are large and streamed. Mixing them bloats the database, complicates deduplication and garbage collection, and enlarges the corruption blast radius.

## Decision

SQLite in WAL mode holds metadata and indices only; binary payloads live in the content-addressed artifact store (ADR-010). Rationale: corruption isolation (payload corruption cannot take down the metadata database), garbage collection and deduplication operate on artifacts, and payloads stream without loading the database. project.db enforces foreign keys across tables and supports migrations driven by schema_version, executed transactionally: each migration runs in a single transaction and is rolled back on failure.

## Alternatives

- Blobs in SQLite: rejected. Database bloat, no streaming, poor GC and dedup, larger corruption blast radius.
- No SQLite (plain JSON files): rejected. No queryability, no transactions, no foreign keys, poor concurrency.
- One metadata database per concern: rejected. Unnecessary complexity for M0; one WAL database with clear table namespaces suffices.

## Consequences

- Positive: isolation of corruption, streaming payloads, dedup and GC on artifacts, transactional migrations, queryable metadata.
- Negative: two sources of truth to keep consistent; migration discipline required; WAL files grow without checkpoints.
- Risks and mitigations: migration tests cover schema changes; integration tests cover corrupted artifacts (payload vs. metadata); regular WAL checkpointing; consistency between project.db and the artifact store is validated by tests.

## References

- docs/specifications/project-format.md
- docs/specifications/artifact-format.md
- ADR-008 (Project storage format)
- ADR-010 (Content-addressed artifact store)
