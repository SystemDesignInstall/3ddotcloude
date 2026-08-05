# RFC-0003 — Processing Engine & Execution Architecture

- **Status:** ratified
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-05
- **Supersedes:** none (extends RFC-0001/RFC-0002 baseline)
- **Depends on:** RFC-0001, RFC-0002, ADR-011, ADR-012, ADR-014, ADR-015, ADR-020, ADR-021, ADR-026, ADR-028, ADR-031, ADR-034, ADR-037
- **Protected surfaces touched:** Engine Execution (`engine/**` — new Constitution §2 surface, ADR-038), Capability API (`schemas/**` — `0003_scheduler.sql` migration, SCHED_*/WORKER_* error codes), Artifact Format (`core/artifacts/**` — task cache integration), UUID (`core/**` type `Uuid` usage in task ids)

## Summary

RFC-0003 moves the Spatial Platform from a **permanent spatial data model** (RFC-0002) to an **executable spatial computing platform**. It introduces the Processing Engine — the runtime layer between the data model and the algorithms — which owns process description (Task model), workflow→Task-DAG derivation, DAG scheduling, task lifecycle and retry/cancellation, persisted run state with restart/resume, a content-addressed task cache, isolated worker process supervision, and execution provenance records. The engine is bounded by ADR-038: it never owns algorithms, storage, GPU management, or UI. Pipeline/Recipe (ADR-026) and Workflow (ADR-028) are ratified here as **model surfaces**; their execution is deferred past M0. The MVP (P1.1–P1.4, Core Platform lane) implements the Task model, TaskGraph DAG, scheduler, cache, persistence, worker protocol with a demo Python worker, the mock photogrammetry pipeline, and a minimal CLI.

## Motivation

The P0/P1/P2a milestones produced immutable, versioned spatial data — Scene, observations, geometry, artifacts — but no way to compute on it. A commercial spatial platform (RealityCapture, Metashape, Leica Cyclone) is defined by *processing*: import → calibrate → reconstruct → export, across hours-long runs that cross process crashes, restarts, and iterative refinement. The pieces already ratified constrain this layer tightly: process isolation (ADR-011), a versioned IPC protocol (ADR-012), durable scheduler state and a reproducibility-critical cache (ADR-020), recipes as versioned pipeline definitions (ADR-026), and the M0 boundary (ADR-031). What is missing is a single ratified model of the execution layer that teams build against — today `engine/` exists as empty directories, the worker protocol has no implementation, and there is no task model beyond `task-model.md`. Without RFC-0003, each team would invent its own execution semantics, and the reproducibility contract (Principle 6) would be unenforceable.

## Goals

- Ratify the Processing Engine as a Constitution-protected surface (`engine/**`, ADR-038) with unambiguous responsibility boundaries.
- Define the Task model as two artifacts: `TaskDefinition` (what to do) and `TaskInstance` (a concrete run).
- Define the TaskGraph DAG semantics: validation rules (acyclicity, type-match, resource feasibility) and failure propagation (default `skipped`).
- Ratify the six-state task lifecycle, recoverable-only retry with bounded backoff, and first-class persisted cancellation.
- Ratify the full ADR-020 cache key (input content hashes + config hash + producer/algorithm version + engine git commit) with `deterministic` and `cache_policy`.
- Ratify the worker model: `WorkerHandle` contract, `ProcessExecutor` (child process, Protobuf IPC), `InProcessExecutor` (mock, same contract), and the demo Python worker.
- Ratify `ExecutionRecord` provenance (inputs, outputs, environment, hardware, start/end).
- Ratify Pipeline (ADR-026 Recipe) and Workflow (ADR-028) as model surfaces, explicitly deferred for execution.
- Define the M0/P1 MVP scope (P1.1–P1.4) and acceptance criteria.

## Non-Goals

- **No algorithm implementations.** COLMAP/OpenMVS/GTSAM/KISS-ICP/Open3D/Gaussian/NeRF/AI workers appear only as mocks (ADR-021) or deferred (P3).
- **No production local executor.** Execution is process-isolated (ADR-011); the in-process executor exists solely as a test/mock behind the same `WorkerHandle` contract.
- **No GPU/accelerator management.** The resource model tracks VRAM budgets; allocation and per-GPU scheduling are deferred (process-model §5).
- **No workflow/pipeline execution.** ADR-028 workflow stages and ADR-026 recipe orchestration are deferred post-M0; M0 executes a directly-specified Task DAG.
- **No remote/cluster/distributed workers.** Deferred (ADR-011 §6); the protocol seam is preserved.
- **No CLI workflow editor/checkpoint UI.** The CLI exposes `spatial run <dag>` and status only.

## Design

### 5.1 Responsibility boundaries (ADR-038)

| Component | Owns | Does not own |
|---|---|---|
| `engine/task` | TaskDefinition, TaskInstance, TaskGraph (DAG) | algorithm logic |
| `engine/scheduler` | DAG validation, ordering, dispatch, state machine, retry, cancellation, persistence of run state | algorithms, storage, GPU management |
| `engine/execution` | ExecutionRecord, provenance records | — |
| `engine/workers` | WorkerHandle, ProcessExecutor, InProcessExecutor, protocol framing, demo worker | algorithm implementations |
| `engine/resources` | ResourceSpec/ResourceProfile, capability matching | actual device allocation |
| `engine/cache` | cache keying (ADR-020), lookup/store against CAS | artifact storage (CAS owns bytes) |
| `engine/pipeline` | PipelineDefinition (← Recipe, ADR-026), PipelineRegistry — **model surface only in M0** | stage execution |
| `engine/workflow` | Workflow model surface (ADR-028) — **placeholder in M0** | stage execution |

### 5.2 Task model

`TaskDefinition` — the reusable description of an operation:

```cpp
struct TaskDefinition {
  TaskType      type;        // semantic string; selection by capability (ADR-011/034)
  InputSchema   inputs;      // declared input artifact kinds
  OutputSchema  outputs;     // declared output artifact kinds
  ParameterSchema parameters; // JSON Schema for the task's configuration
  ResourceSpec  requirements; // cores, RAM, GPU/VRAM, temp-disk bytes (typed)
};
```

`TaskInstance` — one concrete run:

```cpp
struct TaskInstance {
  TaskId        id;          // Uuid, immutable
  TaskDefinition definition;
  std::vector<ArtifactRef> inputs;     // CAS SHA-256, never paths (ADR-010)
  std::vector<ArtifactRef> outputs;    // declared outputs
  TaskStatus    state;       // six-state machine (§5.4)
  TaskMetadata  metadata;    // retry_policy, cancellation_policy, cache_policy,
                             // deterministic flag, attempts, timestamps
};
```

### 5.3 TaskGraph (DAG)

- Tasks are nodes; `dependencies[]` are edges. The graph must be a DAG.
- Validation before dispatch: **(1)** acyclicity (a topological order exists); **(2)** type-match — every input of a task is an `expected_output` of exactly one dependency or a declared external input; **(3)** resource feasibility — each task's requirements, and the union of co-running tasks, fit the worker's advertised capabilities.
- Failure propagation: when a dependency fails, dependents become `skipped` (default) or `failed` (strict path) per a per-task policy.
- The TaskGraph is the unit of work the M0 CLI submits (`spatial run <dag>`).

### 5.4 Task state machine

Single source of truth — the six lowercase states of ADR-020/task-model.md:

```
pending → running → succeeded
          running → failed → pending   (retry, recoverable only; attempts < max)
          pending → cancelled
          pending → skipped            (dependency failed; default propagation)
```

- There is no `created`/`ready`/`queued`/`retrying` state: a task not yet dispatched is `pending`; retry is an attempt counter on a `failed` task that returns to `pending`.
- Exactly one terminal state per task (`succeeded`/`failed`/`cancelled`/`skipped`), enforced as a property invariant.

### 5.5 Retry

- `RetryPolicy { max_attempts, base_ns, multiplier, max_ns }`; default `max_attempts = 2`.
- **Only recoverable failures (ADR-014) are retried**, with bounded exponential backoff. Deterministic/permanent failures (bad inputs, validation errors) never retry.
- Retried tasks re-run from scratch in a fresh deterministic workspace.

### 5.6 Cancellation

- Cooperative: the scheduler sends `TaskCancelled` with a reason; the worker stops at its next safe checkpoint and reports the terminal state.
- `cancellation_policy = cooperative | best_effort`; `best_effort` permits finishing a short critical section.
- **Cancellation is a first-class persisted state**: cancelled tasks are never re-run on resume unless explicitly re-queued (ADR-020).
- Partial outputs of cancelled tasks are discarded; as unreferenced CAS objects their collection is safe (ADR-010).

### 5.7 Workers

- `WorkerHandle` — the contract between scheduler and a worker: `capabilities()`, `submit(TaskRequest)`, `cancel(task_id, reason)`, `shutdown()`, plus an event/result channel.
- `ProcessExecutor` — real M0 execution backend: spawns a child process, Protobuf IPC framed `[u32 LE length][proto bytes]` over stdin/stdout, heartbeat (5 s default), timeout, crash detection (exit/EOF/missed heartbeat → `interrupted`), cancellation delivery, deterministic temp workspace `temp/<job>/<task>`, cleanup on completion/cancellation/crash.
- `InProcessExecutor` — mock/fake worker behind the same `WorkerHandle` contract, used by scheduler unit tests (ADR-021). It is **not** a production execution path.
- Demo worker: `engine/workers/python/demo_worker.py` implements worker-protocol §8 end-to-end (handshake, capabilities, task, progress 0→100%, artifact handoff, heartbeat, shutdown).

### 5.8 Resources

- `ResourceSpec` (per task): cores, RAM bytes, GPU/VRAM bytes, temp-disk bytes.
- `ResourceProfile` (per worker): advertised capacity + `max_concurrency` + capability list.
- The scheduler is the **single allocator** (process-model §5); workers never claim resources themselves. Selection is by capability, not by task name (ADR-011/034).

### 5.9 Artifact integration

- Tasks exchange data **only via content-addressed `ArtifactRef` (CAS SHA-256)** — never by path, never by embedding bytes (ADR-010, worker-protocol §3).
- The scheduler resolves input refs in the store before dispatch and registers output refs on `TaskArtifactProduced`.
- Worker workspaces are ephemeral; produced artifacts are handed back as CAS refs.

### 5.10 ExecutionRecord / Provenance

```cpp
struct ExecutionRecord {
  ExecutionId    id;
  TaskId         task;
  std::vector<ArtifactRef> inputs;
  std::vector<ArtifactRef> outputs;
  SoftwareEnvironment environment;  // engine version, git commit, protocol version
  HardwareInfo   hardware;          // CPU/RAM/GPU of the worker, os/arch
  TimestampNs    started;
  TimestampNs    finished;
  TaskStatus     terminal_state;
  ErrorInfo      error;             // ADR-014 structured error, if failed
};
```

- Persisted per run in `task_runs` (project.db, metadata only); enables reproducibility, audit, comparison (ADR-029), and benchmark evidence.

### 5.11 Cache

- **Cache key (ADR-020):** `SHA-256(task_type || sorted(input CAS content hashes) || effective_config_hash || producer/algorithm_version || engine_git_commit)`.
  - Uses input **content hashes** (not artifact ids) so any input change invalidates.
  - `engine_git_commit` is the git commit of the producing code, embedded at build time (configure step) with a stable fallback.
- `deterministic = true` means identical inputs must produce byte-identical outputs; **only deterministic tasks are served from cache**; non-deterministic tasks always re-run.
- `cache_policy = cacheable | never` — orthogonal axis; `never` bypasses the cache entirely.
- Cache hits/misses are recorded as structured events with the cache key (ADR-015).
- Artifact bytes live in CAS; cache metadata (key → artifact ref, producer, commit) lives in project.db.

### 5.12 Persistence

- Scheduler state (tasks, runs, dependencies, workers, cache entries) persists in `project.db` (SQLite WAL, metadata only, exactly one writer — `core/storage`).
- Every state transition is recorded transactionally with the artifact refs of outputs.
- After a host restart, the DAG **resumes** from the earliest incomplete task; `running` tasks are reconciled against cache results and worker crash records.
- Migration `0003_scheduler.sql` (new, owns the engine tables; `0001` is P1, `0002` is claimed by RFC-0002):

  1. `tasks` (task_id, job_id, task_type, spec_json, config_hash, cache_policy, deterministic, cancellation_policy, status, retry_policy_json, created_at_ns, updated_at_ns)
  2. `task_runs` (run_id, task_id, attempt, worker_id, started_at_ns, ended_at_ns, terminal_state, error_json, input_refs_json, output_refs_json, environment_json, hardware_json)
  3. `task_dependencies` (task_id, dependency_id) — DAG edges
  4. `workers` (worker_id, name, capabilities_json, resource_profile_json, protocol_version, max_concurrency, last_heartbeat_ns, status)
  5. `cache_entries` (cache_key, artifact_id, task_type, producer_version, git_commit, config_hash, created_at_ns, status)

### 5.13 Pipeline & Workflow surfaces (deferred execution)

- **Pipeline** (ADR-026 Recipe): `name`, `version`, `git_commit`, parameter JSON Schema, ordered `stages`, validated parameters, `cache_identity`. `engine/pipeline` ratifies `PipelineDefinition` (derived from a Recipe) and `PipelineRegistry` as model surface; recipe library storage, stage orchestration, and recipe-driven CLI ship post-M0 (P4).
- **Workflow** (ADR-028): ordered user-facing stages Import → Validate → Optimize → Review → Improve → Finalize, derived into a TaskGraph for the scheduler. Ratified as a model surface; the workflow layer ships post-M0 (Photogrammetry milestone). M0 works on directly-specified Task DAGs.

### 5.14 Error codes

New stable codes in `core/errors/project_error.h` and `schemas/protobuf/errors.proto`:

- `SCHED_*` (domain 8): `SCHED_DAG_CYCLE` (8001), `SCHED_DAG_TYPE_MISMATCH` (8002), `SCHED_DAG_RESOURCE_INFEASIBLE` (8003), `SCHED_TASK_UNKNOWN` (8004), `SCHED_PERSISTENCE` (8005), `SCHED_CACHE_MISS` (8006), `SCHED_CANCELLED` (8007).
- `WORKER_*` (domain 9): `WORKER_PROTOCOL` (9001), `WORKER_HEARTBEAT_TIMEOUT` (9002), `WORKER_CRASHED` (9003), `WORKER_TERMINATED` (9004), `WORKER_BUSY` (9005).

All worker errors carry the ADR-014 `recoverable` flag that drives retry.

### 5.15 Engine directory layout

```
engine/
├── task/          TaskDefinition, TaskInstance, TaskGraph
├── scheduler/     dag, queue, state machine, scheduler_state_store
├── execution/     ExecutionRecord, provenance records
├── workers/       worker_handle, in_process_worker, process_worker, protocol_framing, python/demo_worker.py
├── resources/     ResourceSpec, ResourceProfile, capability matching
├── cache/         task_cache
├── pipeline/      PipelineDefinition (← Recipe), PipelineRegistry — surface only
├── workflow/      placeholder (ADR-028 deferred)
└── intelligence/  placeholder (ADR-027 deferred)
```

## Schema Changes

- **`schemas/database/migrations/0003_scheduler.sql`** — the five engine tables (§5.12), metadata and indices only (Constitution §4).
- **`schemas/protobuf/errors.proto`** — stable string codes for SCHED_* and WORKER_* (domains already exist: 8, 9).
- **`schemas/json/*`** — no new schemas in M0; `worker-capabilities.schema.json` (frozen taxonomy) and `workflow.schema.json` (frozen) remain the contract. Recipe/benchmark schemas stay as ratified (ADR-026, ADR-029).

## Database Migration

`0003_scheduler.sql` applies through the existing runner (ADR-009): the runner owns the transaction, embeds the script at build time, and records schema version 3 in `schema_meta`. `0002` is claimed by RFC-0002 and is not created by this RFC.

## API Impact

- **C++ kernel**: new `spatial_engine` static library under `engine/` linking `spatial_core`; new error codes; task cache integrates with `core/artifacts` (read-only + register refs).
- **Worker protocol**: `worker.proto` unchanged (frozen at RFC-0001); M0 adds codegen + `ProcessExecutor`/`InProcessExecutor` and the demo Python worker.
- **CLI**: `spatial run <dag>` submits and monitors a TaskGraph; `spatial status` shows task states. CLI is the Core Platform P1 lane surface (agent-tasks §1).
- **Python SDK**: not in scope for M0 beyond the demo worker.

## MVP Scope (P1.1–P1.4)

**In M0 (P1, Core Platform lane):** TaskDefinition/TaskInstance, TaskGraph DAG + validation, scheduler (state machine, recoverable-only retry, first-class cancellation, persisted resume), task cache (ADR-020 key, deterministic/cache_policy), `0003_scheduler.sql`, ExecutionRecord, worker protocol (ProcessExecutor + InProcessExecutor + demo Python worker), mock photogrammetry pipeline (Images → Mock Feature Extraction → Mock Reconstruction → Point Cloud), CLI `spatial run`/status.

**Out of M0:** algorithm implementations, production local executor, GPU management, distributed/remote/cluster workers, recipe library + recipe-driven orchestration, ADR-028 workflow layer, AI workers, benchmark harness runs (ADR-029).

## Compatibility

- **Preserved:** RFC-0001 baseline contracts (worker.proto, errors.proto domains, capability taxonomy, workflow.schema.json, project/artifact JSON schemas), RFC-0002 data model, artifact store (ADR-010), strict types (ADR-007/018).
- **Extended:** `engine/**` becomes a Constitution §2 surface (ADR-038); error codes gain SCHED_*/WORKER_*; a new 0003 migration is added (0001/0002 untouched).
- **Supersedes:** none.
- Migration of existing `.spx` projects: schema version 3 applies additively; no data is dropped; existing tables and artifacts remain valid.

## Alternatives Considered

- **UPPERCASE task states (CREATED/QUEUED/RETRYING…):** rejected — contradicts ratified ADR-020 and the frozen lowercase string states in `workflow.schema.json`; `retrying` duplicates attempt counting.
- **Scheduler under `core/scheduler/`:** rejected — contradicts ratified process-model.md (`engine/scheduler`), CONSTITUTION §4 (`engine/**`), and the existing directory structure; `engine/**` is the single Constitution surface.
- **Production local executor in M0:** rejected — violates ADR-011 process isolation and process-model §2; the in-process path exists only as a mock (`InProcessExecutor`).
- **Cache key without git commit / keyed on artifact ids:** rejected — violates ADR-020; content hashes + git commit are required to prevent stale reuse.
- **Full pipeline/workflow execution in M0:** rejected — violates ADR-031/028/026 deferrals; surfaces are ratified, execution ships post-M0.
- **Engine owning storage or GPU:** rejected — duplicates `core/storage`/CAS and the deferred GPU plan (ADR-038, process-model §5).

## Acceptance Criteria

1. **AC-1** A Pipeline definition can be described (model surface, ADR-026 Recipe).
2. **AC-2** A Workflow (ADR-028 model) derives into a TaskGraph; a directly-specified TaskGraph is accepted and validated (acyclic, type-matched, resource-feasible).
3. **AC-3** The scheduler executes a TaskGraph; dependent tasks run in topological order; a failed dependency defaults dependents to `skipped`.
4. **AC-4** A task's outputs are registered in the artifact store as CAS refs; tasks never exchange paths.
5. **AC-5** Every run produces a persisted `ExecutionRecord` (inputs, outputs, environment, hardware, terminal state).
6. **AC-6** After a host restart the DAG resumes from the earliest incomplete task; `running` tasks reconcile against cache/worker records; cancelled tasks are never re-run.
7. **AC-7** `spatial run <dag>` and status reporting work against the mock pipeline end-to-end.
8. **AC-8** A deterministic task whose cache key is unchanged is satisfied from cache without worker execution; any key-component change re-runs it; `cache_policy = never` never consults the cache.

## Implementation Plan

Milestones follow the Core Platform P1 lane (agent-tasks §1, ADR-038); each stage runs all gates and Debug/Release 100% test pass:

- **P1.1 Execution Core:** `engine/task` (TaskDefinition/TaskInstance/TaskGraph + validation), `engine/resources`, `engine/execution` (ExecutionRecord), migration `0003_scheduler.sql`, SCHED_*/WORKER_* codes. Property tests: graph acyclicity/type-match/resource-feasibility, state-machine invariants.
- **P1.2 Scheduler Runtime:** `engine/scheduler` (state machine, queue, retry/cancel, `scheduler_state_store`), `engine/cache` (ADR-020 key). Unit tests: transitions, retry recoverable/non, cancellation, resume, cache invalidation.
- **P1.3 Workers:** `engine/workers` (WorkerHandle, ProcessExecutor, InProcessExecutor, protocol framing), demo Python worker, Protobuf codegen. Integration tests: C++ scheduler ↔ Python worker end-to-end, crash mid-task, heartbeat timeout, cancel mid-task, restart/resume.
- **P1.4 First Pipeline:** mock photogrammetry pipeline (Images → Mock Feature Extraction → Mock Reconstruction → Point Cloud), minimal CLI (`spatial run`/status).

## Open Questions

- None blocking. (Future RFC-0004 will ratify the plugin/worker ecosystem beyond the ADR-034/ADR-011 baseline: real adapter packaging, remote/cluster transport, AI worker gating.)

## References

- `docs/architecture/engine.md`, `docs/architecture/scheduler-design.md` (Spatial Platform)
- `docs/specifications/task-model.md`, `docs/specifications/worker-protocol.md`
- `docs/architecture/process-model.md` (ratified P0), `docs/architecture/storage-model.md`
- ADR-011/012 (worker isolation and IPC), ADR-014 (errors), ADR-015 (logging/provenance), ADR-020 (scheduler persistence/cache), ADR-021 (mock adapters), ADR-026 (recipes), ADR-028 (workflow), ADR-031 (M0 scope), ADR-034 (capability plugins), ADR-037 (public API stability), ADR-038 (engine boundary)
- `schemas/protobuf/worker.proto`, `schemas/protobuf/errors.proto`, `schemas/database/schema.sql`, `schemas/database/migrations/0001_init.sql`
- CONSTITUTION.md §1 (principles), §2 (protected surfaces), §4 (constraints), §5 (change control)
