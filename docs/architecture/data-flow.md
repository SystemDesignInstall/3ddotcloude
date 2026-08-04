# Data Flow Through the Platform

- **Status:** ratified (P0)
- **References:** ADR-008/009/010 (storage, metadata, CAS), ADR-011/012 (workers, IPC), ADR-020 (scheduler cache), ADR-024/032 (graphs), ADR-026 (recipes), ADR-028 (workflow), ADR-033 (scene versions)
- **Consistent with:** `docs/architecture/system-overview.md`

## 1. The pipeline at a glance

```
Capture / Import ──► Importer ──► Artifacts (CAS) ──► Scene graphs
   (files)          validate,        SHA-256,          Observation /
                    hash,            immutable         Geometry /
                    provenance                        Relationship
                                                           │
                                                           ▼
                                                   Pipeline DAG (recipe)
                                                           │
                                                           ▼
                                                   Scheduler ──► Workers
                                                   (cache,        (COLMAP,
                                                    durability)    OpenMVS,
                                                        │          GTSAM, AI)
                                                        ▼
                                        New artifacts + new Scene version ──► Export
```

## 2. Capture / Import

- Any sensor data (images, video, point clouds, ROS bags, vendor packages) enters as **raw external files**. Captures may come from the CLI, the SDK, or a capture device package.
- Originals are treated as **immutable read-only facts**. The importer never mutates a source file.

## 3. Importer

The importer is the ingestion adapter per source family (`importers/images`, `importers/video`, `importers/las`, `importers/e57`, `importers/rosbag`, `importers/dji`, ...). For every file it:

1. **Validates** structure and readability (unsupported/corrupt files become `failed` import tasks, never an abort).
2. **Hashes every byte** at ingestion (SHA-256) — this hash is the artifact identity.
3. **Writes provenance**: file path, mtime, importer version + git commit, original checksum.
4. **Stores the exact byte copy** as a CAS artifact (never re-encoded), referenced by the Scene.

Output: a new **scene version** with `ImageObservation` / `LiDARObservation` / `IMUObservation` / `GNSSObservation` / `DepthObservation` records, each with a resolvable `artifact_ref`.

## 4. Artifacts (CAS)

- Payload bytes live in the **content-addressed store** (`artifacts/cas/<hash[0:2]>/<sha256>`), immutable and deduplicated by SHA-256 (ADR-010).
- Each artifact has a **UUID manifest** (`artifacts/<uuid>/manifest.json`) recording content hash, producer, input artifact hashes, configuration hash, git commit, timestamps — the provenance graph.
- Metadata (what points to what) goes to SQLite; **no large binary ever lives in the database** (ADR-009).

## 5. Scene graphs

The Scene is the domain root. Three canonical graphs (ADR-024, ADR-032):

- **Observation Graph** — what was measured, by whom, when; populated at import and immutable thereafter.
- **Geometry Graph** — `GeometryElement`s (Point/Triangle/Voxel/Gaussian, ...) and their containment/registration/LoD edges; grown by reconstruction stages.
- **Relationship Graph** — links between observations, geometry, and semantic objects (base structure only in M0).

Each stage consumes a scene version and produces a **new scene version** (ADR-033). AI outputs enter **only as priors on observations** through the ADR-006 validation gate, never as authoritative geometry.

## 6. Pipeline DAG

- A **Recipe** (ADR-026) is resolved into a **DAG of tasks**, each task declaring the **Capability** it needs (`SparseReconstruction`, `DenseStereo`, `BundleAdjustment`, `ICP`, `SurfaceReconstruction`, `Texturing`, `GaussianGeneration`, `LidarOdometry`, `LoopClosure`, `GnssIntegration`).
- The engine selects implementations **by capability**, never by vendor name (ADR-034). DAG validation rejects cycles, orphaned nodes, and invalid transitions (fuzzed in tests).

## 7. Scheduler

- Owns task states `pending / running / succeeded / failed / cancelled / skipped`, persisted transactionally in `project.db` (ADR-020).
- **Cache check happens here and short-circuits the whole downstream:** before a task is dispatched, the scheduler computes the cache key from input artifact hashes + configuration hash + producer version + git commit. On a **cache hit**, the task is marked `succeeded` with no worker dispatch; its output artifact references are reused and the cache hit is recorded as a structured event (provenance = synthetic producer).
- On a cache miss, tasks are dispatched to workers; recoverable failures retry with bounded backoff; cancellation is persisted and never re-run on resume.
- Retries, resume, and cache hits are the three ways work is **skipped**; `skipped` is the fourth task state (e.g. `Review` on non-final pipeline legs).

## 8. Worker processes

- Each task runs in an **isolated worker process** (ADR-011) over the Protobuf IPC protocol (ADR-012): `WorkerHello`, `WorkerCapabilities`, `TaskRequest`, `TaskProgress`, `TaskLog`, `TaskArtifactProduced`, `TaskCompleted`, `TaskFailed`, `TaskCancelled`, `Heartbeat`, `Shutdown`.
- Workers stream progress/logs, hand produced artifacts back by **hash reference** (never embedded bytes), and are supervised by heartbeat + pipe-closure detection.
- A worker crash is a recoverable failure: the task is retried (or resumed from persisted state) by the scheduler.

## 9. New artifacts + new scene versions

- Every completed task produces **new CAS artifacts**; their manifests record producer, inputs, config, and commit.
- The stage then commits a **new immutable Scene version** (`v → v+1`), advancing the version pointer (ADR-033). Snapshots are cheap because versions reference immutable artifacts and CAS dedupes.

## 10. Export

- Finalize produces exportable products through exporters/adapters: point cloud channels, mesh buffers, Gaussian splat buffers, trajectories (see `docs/formats/README.md`), referencing CAS artifacts by `Uri`.
- Exports are read-only views over committed scene versions; they never mutate the Scene.

## 11. Where cache hits short-circuit

1. **Task level (scheduler):** a deterministic task with a valid cache key completes with **no worker dispatch** (§7). This is the main short-circuit — most iterative workflow time is saved here.
2. **CAS dedup:** identical artifact bytes across tasks/projects are stored once (ADR-010); writers skip redundant writes.
3. **Stage level (workflow, post-M0):** replaying a workflow stage with identical inputs+recipe reuses the stage's task cache entries (ADR-028).

## References

- `docs/architecture/process-model.md`, `docs/architecture/storage-model.md`
- `docs/specifications/reconstruction-pipeline.md`, `docs/specifications/scene-model.md`
- `docs/specifications/recipe-model.md` (recipe → cache-key relationship)
