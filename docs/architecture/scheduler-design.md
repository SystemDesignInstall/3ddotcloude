# Scheduler Design

- **Status:** ratified (P1, RFC-0003)
- **References:** ADR-020, ADR-014, ADR-012, ADR-011, RFC-0003
- **Protected surface:** `engine/scheduler/**`
- **Consistent with:** `docs/architecture/engine.md`, `docs/specifications/task-model.md`

This document details the scheduler: DAG execution, the task state machine, retry and cancellation, persistence and restart/resume, cache-keying, and the worker boundary. It is normative for `engine/scheduler/**`.

## 1. Inputs and outputs

- **Input:** a validated `TaskGraph` (nodes = tasks, edges = dependencies). For M0 the DAG is supplied directly (`spatial run <dag>`); in later milestones a Workflow (ADR-028) derives it.
- **Output:** persisted run state in `project.db` (tables `tasks`, `task_runs`, `task_dependencies`, `workers`, `cache_entries` from migration `0003_scheduler.sql`) plus produced CAS artifact refs.

## 2. Dispatch loop

1. **Validate** the graph once: acyclicity (topological order exists), type-match (every input is an `expected_output` of exactly one dependency or a declared external input), resource feasibility (per-task and co-running union fit a worker's advertised profile).
2. **Ready set:** tasks whose dependencies are all `succeeded` become `pending`.
3. **Dispatch:** the scheduler selects a worker by capability (ADR-011/034) whose profile fits the task requirements, respecting `max_concurrency`. A deterministic task whose cache key hits is satisfied from cache without dispatch.
4. **Track:** on `TaskArtifactProduced`, register the ref with the CAS index; on terminal message, persist the run record and advance the ready set.
5. **Complete:** the graph is complete when every node reached a terminal state.

## 3. State machine

Six lowercase states (ADR-020); no extra `created`/`queued`/`retrying` states.

```
pending ──► running ──► succeeded
   │          │
   │          └──────► failed ──► pending   (retry: recoverable only, attempt < max)
   │
   ├────────► cancelled                (first-class, persisted; never re-run on resume)
   └────────► skipped                  (dependency failed; default propagation)
```

Invariant (property-tested): every task reaches **exactly one** terminal state (`succeeded | failed | cancelled | skipped`), recorded transactionally.

## 4. Retry policy

- `RetryPolicy { max_attempts = 2, base_ns, multiplier, max_ns }`, bounded exponential backoff.
- Only failures whose `ErrorInfo.recoverable == true` (ADR-014) retry: transient IPC loss, resource contention, worker crash. Deterministic failures (bad inputs, validation errors) never retry.
- A retried task re-runs from scratch in a fresh deterministic workspace (`temp/<job>/<task>`), which is created before dispatch and removed on completion/cancellation/crash.

## 5. Cancellation

- Cooperative: `TaskCancelled` (with reason) → the worker stops at its next safe checkpoint and reports the terminal state. `cancellation_policy = cooperative | best_effort`.
- If the worker does not acknowledge within the timeout, the scheduler escalates to `Shutdown`, then terminates the process.
- Partial outputs are discarded; as unreferenced CAS objects they are collectable (ADR-010).
- **Cancelled is a persisted terminal state**: on resume, cancelled tasks are never re-run unless explicitly re-queued.

## 6. Persistence and resume

- All transitions are persisted in the same transaction as the run record (single SQLite writer, WAL, metadata only).
- On host restart: load `tasks`/`task_dependencies`/`task_runs`; reconcile `running` tasks against cached results and worker crash records; resume from the earliest incomplete task (ADR-020).

## 7. Cache integration

`engine/cache` computes the ADR-020 key: `SHA-256(task_type + sorted input content hashes + config_hash + producer_version + engine_git_commit)`.

- Deterministic + `cache_policy = cacheable` → look up; on hit, mark the task `succeeded` with the cached output refs and record a cache event.
- Any key component change (input content, config, producer version, git commit) invalidates.
- `cache_policy = never` never consults the cache; non-deterministic tasks always re-run.

## 8. Worker boundary

- `WorkerHandle` contract: `capabilities()`, `submit(task_request)`, `cancel(task_id, reason)`, `shutdown()`, plus an event/result channel (progress, log, artifact, terminal).
- `ProcessExecutor`: spawns a child process, Protobuf IPC framed `[u32 LE length][proto]` over stdin/stdout, heartbeat (5 s), crash detection (exit / EOF / missed heartbeat → `interrupted`), cleanup.
- `InProcessExecutor`: mock behind the same contract for unit tests (ADR-021); not a production path.
- The scheduler never embeds algorithm logic; it selects by capability.

## 9. Fault injection and testing (ADR-016)

- Lost worker, kill during write, heartbeat timeout, cancellation during a task.
- Integration: C++ scheduler + demo Python worker end-to-end, crash mid-task, restart/resume, worker crash mid-task.
- Property tests: single terminal state per task; cache-key equality ⇔ component equality; TaskGraph round-trip.

## References

- `docs/architecture/engine.md`, `docs/architecture/process-model.md`
- `docs/specifications/task-model.md`, `docs/specifications/worker-protocol.md`
- RFC-0003, ADR-011, ADR-012, ADR-014, ADR-015, ADR-020, ADR-021
