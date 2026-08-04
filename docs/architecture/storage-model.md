# Storage Model

- **Status:** ratified (P0)
- **References:** ADR-008 (project storage format), ADR-009 (SQLite metadata separation), ADR-010 (content-addressed artifact store), ADR-020 (scheduler persistence)
- **Consistent with:** `docs/specifications/project-format.md`

## 1. Principle

**No large binaries in SQLite.** SQLite holds metadata and indices only; every payload lives in the content-addressed artifact store. This isolates corruption blast radius, enables streaming, and makes dedup/GC operate on artifacts, not rows.

## 2. Project layout (.spx)

A project is a directory carrying the `.spx` extension with exactly six entries (ADR-008):

```
my-project.spx/
├── project.json   # stable manifest, human-readable (Section 3)
├── project.db     # SQLite, WAL mode, metadata + indices only (Section 4)
├── artifacts/     # content-addressed artifact store (Section 5)
├── cache/         # scheduler task cache (Section 6)
├── logs/          # JSON-lines structured logs (Section 7)
└── temp/          # deterministic worker temp workspaces (Section 8)
```

The layout is an implementation detail of the storage layer; no other code (including plugins) may hard-code paths below `.spx`.

## 3. project.json

- The minimal, stable, human-auditable manifest. The only file read before opening the database.
- Fields (exact): `uuid`, `name`, `schema_version`, `created_by {app, version, git_commit}`, `created_at` (ISO-8601 UTC), `default_crs`, `root_frame`, `flags {read_only, encrypted}`, `properties`.
- `uuid` is identity and never changes; `name` is display-only; `schema_version` bumps only on breaking layout/DB changes.
- Unknown extra fields are tolerated (forward compatibility). **No absolute local paths are ever persisted** — all references use the portable `Uri` abstraction (`cas://<sha256>`), so projects are movable and shareable.

## 4. project.db (SQLite metadata)

- **WAL mode.** One writer (Core); readers never block writers. WAL files are checkpointed regularly.
- **`schema_version` table** (single row) plus a `migrations` table listing applied migration ids.
- **Operational tables:** `sessions`, `sensors`, `frames`, `poses`, `observations`, `geometry`, `jobs`, `tasks`, `artifacts_index`, `provenance` (scene/obs/pose/geom tables are prefixed accordingly).
- **`artifacts_index`** maps content hashes → artifact UUIDs → locations in `artifacts/`.
- **`provenance`** stores the edges of the provenance graph.
- **Migrations** are driven by `schema_version`, executed transactionally — each migration runs in a single transaction and is rolled back on failure. Migration tests cover schema changes.
- **Locking:** at most one writer per project, enforced with platform-native file locking inside the `.spx` directory; concurrent writers are refused; read-only openers never block writers.
- **Read-only mode:** set via `flags.read_only`. The storage layer refuses all writes, including cache and log writes (logs are skipped rather than redirected).
- No cache data is stored in the database; the cache lives in `cache/` (Section 6).

## 5. Artifact store (CAS)

- **Content addressing:** payloads are addressed by **SHA-256**; layout `artifacts/cas/<hash[0:2]>/<sha256>` (two-character shard prefix).
- **UUID manifests:** metadata lives at `artifacts/<uuid>/manifest.json`, recording the artifact's UUID, content hash, producer, **input artifact hashes**, configuration hash, git commit, and timestamps — forming the provenance graph.
- **Immutability:** artifacts are written once and never modified; identical content deduplicates to a single CAS entry.
- **Atomic writes:** write to a temp file in the same directory, `fsync`, then **rename into place**. A manifest referencing a missing payload, or a payload with no manifest, is corruption.
- **Garbage collection:** an API removes unreferenced artifacts; integrity validation verifies hashes on read and during GC. GC operates on artifacts (reference tracking across manifests and the scheduler cache), never on SQLite rows.
- **Deduplication:** identical bytes across tasks, versions, and projects are stored once — this is what makes immutable scene versions cheap (ADR-033).

## 6. cache/

- Scheduler task cache entries. Keys are computed from **input artifact hashes + configuration hash + producer version + git commit** (ADR-020); any change to a component invalidates the entry.
- Each entry is self-contained and records its key in its metadata; cache hits/misses are logged as structured events (ADR-015).

## 7. logs/

- Append-only JSON-lines files, one object per line: at least `timestamp`, `level`, `module`, `session_id`, `message`.
- Worker stderr is captured here as structured `task_log` records. Logs are rotated by size, may be discarded, and are **never part of project provenance**.

## 8. temp/

- Deterministic worker workspaces: one directory per task, `temp/<job_id>/<task_id>`.
- Created by the scheduler, owned by the worker for the task's lifetime, removed on completion/cancellation/crash; anything left at project close is deleted.

## 9. Atomicity and recovery

- **Open/create:** validates `project.json`, runs an integrity check on `project.db`, verifies manifest/payload consistency in `artifacts/`.
- **Atomic save:** every mutation follows temp-then-rename with `fsync`; readers observe either the old or the new complete state, never a partial one.
- **Recovery:** an interrupted transaction is detected via WAL state and the schema-version journal; the next open rolls back incomplete mutations and cleans orphaned `temp/` and `cas/` files.
- **Integrity check:** verifies every artifact hash and every DB foreign key; failures are reported with the offending hash/UUID; the project is **never silently repaired**.

## 10. Portability

No absolute local paths in any persisted state. A project may be copied, moved across filesystems, or opened from a read-only volume without changing any persisted bytes.

## References

- `docs/specifications/project-format.md`, `docs/architecture/data-flow.md`
- ADR-008, ADR-009, ADR-010, ADR-020
