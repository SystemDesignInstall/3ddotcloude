# Process Model

- **Status:** ratified (P0)
- **References:** ADR-011 (process worker isolation), ADR-012 (IPC protocol), ADR-008 (project layout), ADR-020 (scheduler persistence)
- **Consistent with:** `docs/architecture/system-overview.md`

## 1. Two process roles

```
┌────────────────────────── HOST PROCESS ──────────────────────────┐
│  Core (libspatial_core)      Scheduler (engine/scheduler)        │
│  · project / storage         · DAG state, retries, cache (ADR-020)│
│  · Scene + graphs            · resource allocation               │
│  · strict types/coordinates  · worker supervision                │
│  · SDK/CLI surface           · persistence (project.db)          │
└──────────────────────────────┬───────────────────────────────────┘
                               │ Protobuf IPC over stdin/stdout pipes
                               │ (ADR-012)
                ┌──────────────┴──────────────┐
                ▼                             ▼
        ┌───────────────┐            ┌───────────────┐
        │ WORKER (task) │            │ WORKER (task) │   ... N workers
        │ COLMAP / MVS  │            │ GTSAM / AI    │
        │ GTSAM / KISS  │            │ ...           │
        └───────────────┘            └───────────────┘
```

- **Host process:** runs Core and the scheduler. Owns `project.db`, the CAS, the Scene, and all coordination. Exactly **one writer** for the SQLite database.
- **Worker processes:** isolated, one per task, child processes of the host. Each runs a third-party backend or an AI model behind an adapter.

## 2. Isolation rationale

Every external component (COLMAP, OpenMVS, GTSAM, Open3D, KISS-ICP, VGGT, gsplat, ...) runs in its own process (ADR-011). This gives:

- **Crash isolation** — a segfault or OOM in a backend cannot take down Core or corrupt the project.
- **Per-process resource limits** — CPU/RAM/GPU enforced at process level, so one runaway backend cannot starve the host.
- **Restart capability** — a dead worker is replaced by a fresh process running the same task.
- **Deterministic temp workspace** — each worker gets `temp/<job_id>/<task_id>`; stdout/stderr are captured into structured logs.

In-process backend calls and thread-based concurrency were rejected: third-party C/C++ and Python state makes isolation impossible, and a crash in-thread kills the host.

## 3. Protocol (ADR-012)

- Transport: **child-process stdin/stdout pipes**, framed as `[u32 little-endian length][protobuf bytes]`. Platform-uniform on Windows and Linux; no shared memory assumption.
- Messages: `WorkerHello`, `WorkerCapabilities`, `TaskRequest`, `TaskAccepted`, `TaskProgress`, `TaskLog`, `TaskArtifactProduced`, `TaskCompleted`, `TaskFailed`, `TaskCancelled`, `Heartbeat`, `Shutdown`.
- **Versioned:** the worker sends `WorkerHello` with its protocol version on startup; version mismatch rejects the worker.
- **No exceptions across IPC.** Errors cross as structured payloads with a stable code, message, context, `recoverable` flag, suggested action, and chained cause (ADR-014; see `docs/architecture/error-model.md`).
- Artifacts are transferred by **hash reference**, never embedded bytes.

## 4. Supervision, crash and restart semantics

- **Heartbeat + timeout:** the host expects periodic `Heartbeat`; a missed timeout, combined with **EOF/pipe closure**, is declared a worker crash.
- **Crash handling:** the task transitions to `failed` with a recoverable `WorkerError` (if the failure is transient) → retried with bounded backoff, or `unrecoverable` → failed and cascaded per the DAG.
- **Cancellation:** delivered as a protocol message and **acknowledged**; cancelled is a persisted first-class state (never re-run on resume unless explicitly re-queued).
- **Safe cleanup:** on completion, cancellation, or crash, the host removes the worker's temp workspace and reaps the process. Zombie reaping is handled explicitly on both Windows and Linux.
- **Restart/resume (ADR-020):** the scheduler persists task state and checkpoint refs transactionally; after a host restart the DAG resumes from the earliest incomplete task, with `running` tasks reconciled against cached results and worker crash records.
- **Recoverable vs deterministic failure:** transient failures (worker crash, I/O) retry; deterministic failures (bad input, bad config) fail with a reproducible report.

## 5. Resource model

| Resource | Accounting | Notes |
|---|---|---|
| CPU | per-process affinity/quota | host decides parallelism; workers are not oversubscribed beyond the declared cap |
| RAM | per-worker ceiling | enforced at process level on the host |
| GPU / VRAM | exclusive or shared per capability | single GPU pass-through per worker for M0; VRAM budget tracked in the resource model for later scheduling |
| Temp disk | `temp/<job_id>/<task_id>` | deterministic per task; deleted after completion/crash; anything left at project close is deleted |
| Project disk | CAS + cache | shared read path; workers write only to `temp/` and hand results back as artifacts |

- The **scheduler is the single allocator**; workers never claim resources on their own.
- Resource availability feeds capability selection (e.g. an AI adapter that requires a GPU is only selected when a GPU is available).

## 6. Distributed future (deferred)

- Cloud/remote workers are **deferred**. The protocol and process boundaries are deliberately transport-agnostic — the worker protocol message set and the capability/descriptor contract are the same whether the worker is a local child process or a remote host.
- When cloud workers arrive, only the **process spawning/supervision layer** changes; Core, adapters, task model, and the wire protocol remain. This is an intentional seam, not a planned rewrite.

## 7. Testing hooks

- Fault-injection: lost worker, kill during write, heartbeat timeout, cancellation during a task (ADR-016).
- Integration: C++ scheduler + Python worker end-to-end, restart/resume, worker crash mid-task.

## References

- `docs/specifications/worker-protocol.md`, `docs/specifications/task-model.md`
- `docs/architecture/error-model.md`, `docs/architecture/storage-model.md`
- ADR-011, ADR-012, ADR-020, ADR-028
