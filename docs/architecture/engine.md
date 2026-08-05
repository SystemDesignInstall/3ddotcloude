# Engine Architecture

- **Status:** ratified (P1, RFC-0003)
- **References:** ADR-011, ADR-012, ADR-014, ADR-020, ADR-021, ADR-026, ADR-028, ADR-031, ADR-034, ADR-038
- **Protected surface:** `engine/**` (CONSTITUTION.md §2, Engine Execution)
- **Consistent with:** `docs/architecture/process-model.md`, `docs/architecture/storage-model.md`, `docs/specifications/task-model.md`, `docs/specifications/worker-protocol.md`

This document describes the **Processing Engine** (RFC-0003): the runtime layer between the permanent spatial data model (RFC-0002) and the algorithms. It turns static, immutable, versioned data into an executable spatial computing platform. The boundary of the engine is defined by ADR-038.

## 1. Position in the platform

```
Permanent Spatial Data Model (RFC-0002)   — Scene, observations, geometry, provenance
                  |
                  v
Artifact Store (CAS, ADR-010)             — immutable, content-addressed payloads
                  |
                  v
Processing Engine (RFC-0003)              — this document
  · process description (Task model)
  · workflow → Task DAG derivation (ADR-028 surface)
  · DAG scheduling, lifecycle, retry, cancellation
  · persisted run state + restart/resume
  · content-addressed task cache
  · worker process supervision (IPC)
  · execution provenance (ExecutionRecord)
                  |
                  v
Algorithms / Workers                       — COLMAP, GTSAM, Open3D, KISS-ICP,
                                             Gaussian/NeRF, AI (mocks in M0)
```

The engine owns neither the algorithms nor the storage. It reads/writes `project.db` and the CAS through `core/storage` and `core/artifacts`, and it selects workers by capability (ADR-011/034).

## 2. Component map

```
engine/
├── task/          TaskDefinition, TaskInstance, TaskGraph (DAG) + validation
├── scheduler/     dag, queue, state machine, scheduler_state_store
├── execution/     ExecutionRecord, provenance records
├── workers/       worker_handle, in_process_worker, process_worker,
│                  protocol_framing, python/demo_worker.py
├── resources/     ResourceSpec, ResourceProfile, capability matching
├── cache/         task_cache (ADR-020 key)
├── pipeline/      PipelineDefinition (← ADR-026 Recipe), PipelineRegistry — surface only
├── workflow/      placeholder (ADR-028 deferred)
└── intelligence/  placeholder (ADR-027 deferred)
```

Dependency direction (CONSTITUTION §5): `engine/workers → engine/scheduler → engine/task → core/*`; `engine/cache → core/artifacts`; `engine/scheduler → core/storage` (persistence).

## 3. Responsibility boundaries (ADR-038)

| Component | Owns | Delegates to |
|---|---|---|
| Task model | TaskDefinition/TaskInstance/TaskGraph | — |
| Scheduler | DAG execution, lifecycle, retry/cancel, resume | storage (persistence), workers (execution) |
| Workers | worker process supervision, IPC framing | adapters/algorithms (never embedded) |
| Cache | cache keying + lookup | CAS (artifact bytes) |
| Resources | typed specs + capability matching | actual device allocation (deferred) |
| Execution | ExecutionRecord provenance | project.db metadata |
| Pipeline/Workflow | model surfaces (M0) | execution (post-M0) |

The engine is the **single allocator** of compute resources; workers never claim resources on their own (process-model §5).

## 4. Execution model

- The scheduler executes a **TaskGraph** (DAG): validated for acyclicity, type-match, and resource feasibility before dispatch (task-model §2).
- Each task follows the six-state machine `pending/running/succeeded/failed/cancelled/skipped` (ADR-020). Retry applies to recoverable failures only, with bounded exponential backoff (default `max_attempts = 2`). Cancellation is cooperative and a first-class persisted state — cancelled tasks never re-run on resume unless explicitly re-queued.
- Tasks exchange data **only via content-addressed `ArtifactRef`s** (CAS SHA-256). Workers write to a deterministic temp workspace `temp/<job>/<task>`, hand results back as refs, and their workspace is cleaned up on completion/cancellation/crash.
- State is persisted transactionally in `project.db` (SQLite WAL, metadata only, exactly one writer). After a host restart the DAG resumes from the earliest incomplete task.

## 5. Task cache

Cache identity is the full ADR-020 composite:

```
cache_key = SHA-256(
    task_type
  + sorted(input CAS content hashes)
  + effective_config_hash
  + producer/algorithm version
  + engine git commit
)
```

- Only `deterministic` tasks are served from cache; non-deterministic tasks always re-run.
- `cache_policy = cacheable | never` bypasses the cache entirely when `never`.
- Hits/misses are recorded as structured events (ADR-015) with the cache key for audit.

## 6. Worker protocol

The engine supervises isolated child processes over Protobuf IPC (ADR-011/012): frames `[u32 LE length][proto bytes]` over stdin/stdout, heartbeat (5 s), timeout, crash detection, cooperative cancellation, and graceful shutdown. The `InProcessExecutor` is a mock/fake worker behind the same `WorkerHandle` contract used by unit tests (ADR-021); it is **not** a production execution path — a production local executor is explicitly out of scope (ADR-038, ADR-011).

## 7. Provenance

Every task run produces an `ExecutionRecord` (task, input/output refs, software environment, hardware, start/end, terminal state, structured error) persisted in `task_runs`. This is the substrate for reproducibility (Principle 6), audit, comparison, and benchmark evidence (ADR-029).

## 8. Deferred subsystems

- `engine/workflow/` — ADR-028 workflow layer (user-facing stages Import→…→Finalize), deferred post-M0; the `workflow.schema.json` contract is already frozen.
- `engine/pipeline/` — ADR-026 recipe library storage and stage orchestration, deferred post-M0 (P4); the model surface and registry ratify now.
- `engine/intelligence/` — ADR-027 adaptive strategy selection, deferred.
- Remote/cluster/distributed workers — deferred (ADR-011 §6); the protocol seam is deliberately transport-agnostic.

## References

- `docs/architecture/scheduler-design.md`, `docs/architecture/process-model.md`
- `docs/specifications/task-model.md`, `docs/specifications/worker-protocol.md`
- RFC-0003, ADR-038, ADR-011, ADR-012, ADR-014, ADR-015, ADR-020, ADR-021, ADR-026, ADR-028, ADR-031, ADR-034
