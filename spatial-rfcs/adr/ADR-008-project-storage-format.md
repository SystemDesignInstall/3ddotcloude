# ADR-008 — Project storage format

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Projects must be self-contained, portable, and resumable. Large binary data must never live in SQLite. The format must distinguish stable, human-auditable manifest content from rapidly changing operational metadata.

## Decision

A project is a directory with extension .spx containing:

- project.json: minimal, stable manifest with uuid, name, schema_version, created_by (app/version/git_commit), created_at, default_crs, root_frame, flags, and properties.
- project.db: SQLite (WAL mode) operational metadata — see ADR-009.
- artifacts/: content-addressed artifact store — see ADR-010.
- cache/: cached task results and intermediate data.
- logs/: structured JSON-lines logs.
- temp/: deterministic temporary workspace.

Saves are atomic: write a complete new project directory or temp file, then rename into place. Persisted state uses a portable Uri abstraction; absolute local paths never appear in persisted state, so projects are movable and shareable. Projects support a read-only mode for inspection and sharing.

## Alternatives

- Single-file database (SQLite with blobs): rejected. No streaming, poor GC and dedup, corruption risk for large payloads, and poor cache isolation.
- Plain directory of files without a manifest: rejected. No schema versioning or created_by provenance.
- Duplicate metadata in XML/YAML: rejected. project.json keeps a minimal stable surface; operational data stays in project.db.

## Consequences

- Positive: portable, resumable, inspectable projects; atomic saves prevent corruption; large data streams from the artifact store; read-only mode supports sharing.
- Negative: a two-file-plus-directories layout is more complex than a single file; rename-based atomicity needs care on all filesystems; cache/ may grow unbounded.
- Risks and mitigations: schema migrations driven by schema_version in project.json and project.db (ADR-009); fault-injection tests (kill during write, disk full) validate atomic-save behavior; portable Uris validated by integration tests.

## References

- docs/specifications/project-format.md
- docs/specifications/scene-model.md
- ADR-009 (SQLite metadata separation)
- ADR-010 (Content-addressed artifact store)
