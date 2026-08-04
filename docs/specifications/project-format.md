# .spx Project Format Specification

Status: Draft
References: ADR-008 (project storage), ADR-009 (artifact store), ADR-010 (content addressing), ADR-033 (atomic save and crash recovery)

This document defines the on-disk format of a Spatial Platform project (`.spx`). It is the single source of truth for how a project is laid out, opened, saved, and validated. The artifact store (see `artifact-format.md`) and the worker protocol (see `worker-protocol.md`) reference this document for path and atomicity conventions.

## 1. Layout

A project is a directory whose name carries the `.spx` extension. Inside it there are exactly six entries:

```
my-project.spx/
├── project.json   # stable manifest, human-readable (Section 2)
├── project.db     # SQLite database, metadata only (Section 3)
├── artifacts/     # content-addressed artifact store (Section 4)
├── cache/         # scheduler task cache (Section 5)
├── logs/          # JSON-lines structured logs (Section 6)
└── temp/          # deterministic worker temp workspaces (Section 7)
```

The platform treats the layout as an opaque implementation detail of the storage layer. No other code — including plugins — may assume or hard-code any path below `.spx`.

## 2. project.json

`project.json` is the minimal, stable manifest. It is the only file the platform reads before opening the database, and its contents are the source of truth for project identity and compatibility. The exact field list is:

| Field | Type | Required | Meaning |
| --- | --- | --- | --- |
| `uuid` | string (UUIDv4) | yes | Globally unique project id. Never changes over the project's lifetime. |
| `name` | string | yes | Display name. May change; it is not an identity. |
| `schema_version` | integer | yes | Project format version. Bumped only on breaking layout/db changes. |
| `created_by` | object | yes | Toolchain that created the project (see below). |
| `created_at` | string (ISO-8601 UTC) | yes | Creation timestamp. |
| `default_crs` | string | yes | Default coordinate reference system, e.g. `EPSG:4326`. |
| `root_frame` | string | yes | Logical root frame id for poses (see project.db `frames`). |
| `flags` | object | no | `read_only` (bool) and `encrypted` (bool). Both default to `false`. |
| `properties` | object | no | Free-form key/value metadata. Never used for semantics. |

`created_by` has the exact fields `app`, `version`, `git_commit`.

Complete example:

```json
{
  "uuid": "f1c9e06e-3b62-4d24-9c3d-7a1b2c3d4e5f",
  "name": "Downtown Survey 2026",
  "schema_version": 1,
  "created_by": { "app": "spatial-platform", "version": "0.1.0", "git_commit": "9a8b7c6d" },
  "created_at": "2026-08-04T09:30:00Z",
  "default_crs": "EPSG:4326",
  "root_frame": "root",
  "flags": { "read_only": false, "encrypted": false },
  "properties": { "site": "downtown", "client": "acme" }
}
```

Unknown or extra fields in `project.json` are tolerated and ignored, preserving forward compatibility.

## 3. project.db

`project.db` is a SQLite database running in WAL mode. It holds **metadata and indices only**; no large binary payloads are ever stored in it. Binary payloads live in `artifacts/` (Section 4) and the database stores only references to them.

- `schema_version` table: single row with the schema version and a `migrations` table listing applied migration ids.
- Operational tables: `sessions`, `sensors`, `frames`, `poses`, `observations`, `geometry`, `jobs`, `tasks`, `artifacts_index`, `provenance`.
- `artifacts_index` maps content hashes to artifact UUIDs and locations in `artifacts/`.
- `provenance` stores the edges of the provenance graph (see `artifact-format.md` Section 7).

No cache data is stored in the database; the cache lives in `cache/` (Section 5).

## 4. artifacts/

Content-addressed artifact store. Payload bytes live under `artifacts/cas/<hash[0:2]>/<sha256>`. Metadata lives under `artifacts/<uuid>/manifest.json`. A manifest that references a missing payload, or a payload with no manifest, is treated as corruption (see `artifact-format.md`).

## 5. cache/

`cache/` holds scheduler task cache entries. Cache keys are computed from input artifact hashes plus the task configuration hash, producer version, and producer git commit. Cache validity is therefore structural: any change to inputs, config, or producer code invalidates the entry. Each entry is self-contained and includes its key in its metadata.

## 6. logs/

`logs/` holds append-only JSON-lines files, one object per line. Each record has at least `timestamp`, `level`, `module`, `session_id` (when applicable), and `message`. Worker stderr is captured here as structured `task_log` records (see `worker-protocol.md` Section 1). Logs are rotated by size and may be discarded; they are never part of project provenance.

## 7. temp/

`temp/` contains deterministic worker temp workspaces: one directory per task, named `<job_id>/<task_id>`. Workspaces are created by the scheduler, owned by the worker process for the task's lifetime, and removed on task completion, cancellation, or crash. Anything left in `temp/` at project close is deleted.

## 8. Atomicity

- **Open/create:** opening a project validates `project.json` (fields above), runs an integrity check on `project.db`, and verifies manifest/payload consistency in `artifacts/`.
- **Locking:** a project may be opened by at most one writer at a time. A lock file inside the `.spx` directory is acquired with platform-native file locking. Concurrent writers are refused; read-only openers never block writers.
- **Atomic save:** every mutation follows the temp-then-rename pattern. Data is written to a sibling `temp/` file, `fsync`ed, then atomically renamed over the target. Readers always observe either the old or the new complete state, never a partial one.
- **Recovery:** an interrupted transaction is detected via WAL state and the `schema_version` journal; on the next open the platform rolls back incomplete mutations and cleans orphaned `temp/` and `cas/` files.
- **Integrity check:** a full check verifies every artifact hash and every database foreign key. Failures are reported with the offending hash/UUID; the project is not silently repaired.
- **Read-only mode:** set via `flags.read_only`. The storage layer refuses all writes, including cache writes and log writes; logs are skipped rather than redirected.

## 9. Portability

No absolute local paths are ever persisted in `project.json`, `project.db`, or any manifest. All references are relative to the `.spx` root or use the portable `Uri` abstraction (e.g. `cas://<sha256>` for payloads). A project may be copied, moved across filesystems, or opened from a read-only volume without changing any persisted bytes.
