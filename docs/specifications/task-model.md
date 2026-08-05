# Task Model Specification

- **Status:** draft (P0)
- **References:** ADR-020, ADR-012, ADR-011, RFC-0003
- **Protected surface:** `engine/task/**`, `engine/scheduler/**`, `engine/workers/**`

This specification defines the **Task**, the unit of work dispatched by the scheduler to workers. Workers are separate processes communicating over Protobuf IPC; the Task model is the contract between the scheduler and a worker. It is normative for `engine/task/**`, `engine/scheduler/**`, and the worker protocol (RFC-0003 ratifies the engine surface).

## 1. Task definition

The Task model has two artifacts (RFC-0003 §5.2): a `TaskDefinition` (the reusable description of an operation) and a `TaskInstance` (one concrete run). The `TaskInstance` is described by this table; the `TaskDefinition` declares its `type`, `inputs`, `outputs`, `parameters` schema, and `requirements`.

| Field | Type | Notes |
|---|---|---|
| `task_id` | Uuid | immutable |
| `task_type` | string | semantic type; worker selection is **by capability**, not by name (ADR-011) |
| `inputs[]` | ArtifactRef[] | immutable CAS inputs (SHA-256, ADR-010) |
| `expected_outputs[]` | ArtifactRef[] | declared outputs the worker must produce |
| `dependencies[]` | TaskId[] | edges in the DAG (§2) |
| `requirements` | ResourceSpec | CPU cores, RAM, GPU, VRAM, temp-disk bytes |
| `retry_policy` | RetryPolicy | §3 |
| `cancellation_policy` | enum | `cooperative, best_effort` (§4) |
| `cache_policy` | enum | `cacheable, never` (§5) |
| `deterministic` | bool | when true, identical inputs must produce byte-identical outputs |

All sizes are typed (bytes, cores, MB/s); task inputs and outputs are content-addressed artifacts, never file paths.

### 1.1 Worker protocol

Tasks travel to workers over Protobuf IPC (ADR-012). The message set:

`WorkerHello`, `WorkerCapabilities`, `TaskRequest`, `TaskAccepted`, `TaskProgress`, `TaskLog`, `TaskArtifactProduced`, `TaskCompleted`, `TaskFailed`, `TaskCancelled`, `Heartbeat`, `Shutdown`.

- A worker advertises **capabilities** (`WorkerCapabilities`), and the scheduler assigns tasks whose `requirements` fit them — selection by capability, not by task name (ADR-011).
- Progress and logs stream via `TaskProgress` / `TaskLog`; produced artifacts are announced with `TaskArtifactProduced`.
- Liveness is enforced by `Heartbeat`; the scheduler uses `Shutdown` for graceful teardown.

## 2. DAG semantics

- Tasks are **nodes**, dependencies are **edges** (`dependencies[]`). The graph is a DAG.
- Validation rules, checked before dispatch:
  1. The graph is **acyclic** (a topological order exists).
  2. **Type match**: each input of a task is an `expected_output` of exactly one dependency (or a declared external input).
  3. **Resource feasibility**: the requirements of every task, and of the union of co-running tasks, fit the worker's advertised capabilities.
- States: `pending, running, succeeded, failed, cancelled, skipped`.
- **Failure propagation**: when a dependency fails, dependents either become `skipped` (producer ignored, consumer not attempted) or `failed` (strict path), per a per-task policy. Default is `skipped`.

## 3. Retry policy

- `max_attempts`: total attempts before a task is declared failed.
- `backoff`: base delay and multiplier (e.g. `{ base_ns, multiplier, max_ns }`) between attempts.
- **Retryable only if recoverable**: the worker's error model marks a failure with a `recoverable` flag. Recoverable failures (transient IPC loss, resource contention, worker restart) are retried; deterministic or permanent failures (bad inputs, validation errors) are not.

## 4. Cancellation

- Cancellation is **cooperative**, driven by the Worker Protocol. The scheduler sends a `TaskCancelled` message to the worker running the task.
- The worker stops at its next safe checkpoint, releases resources, cleans up temp artifacts, and replies with the terminal state.
- `best_effort` policies permit the worker to finish a short critical section before honoring cancellation.
- Cleanup of partial outputs: cancelled tasks may emit partial artifacts; the scheduler discards them. As unreferenced CAS objects, their collection is safe (ADR-010).

## 5. Persistence

- Scheduler state (DAG, task states, worker assignments) is persisted in `project.db` (SQLite metadata only, ADR-020), enabling **resume** of an interrupted run from the last persisted state.
- **Cache keying**: a cacheable task's output is keyed by `(input_hashes, config_hash, producer_version, engine_git_commit)`. Identical keys reuse the cached artifact, skipping execution.
- `deterministic` tasks are always re-executed when inputs or producer versions change; the cache only serves byte-verified matches.

## 6. M0 scope

M0 implements (ADR-031, RFC-0003 P1): the full Task model, DAG construction + validation, state machine, retry and cancellation policy handling, persistence in `project.db`, cache keying, and the complete Worker Protocol — all exercised with **fake/mock workers** (`InProcessExecutor`) that execute against the same IPC contract. Real workers (COLMAP, OpenMVS, etc.) are deferred; they plug into the same protocol.
