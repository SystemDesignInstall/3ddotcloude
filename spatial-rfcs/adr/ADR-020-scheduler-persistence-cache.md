# ADR-020 — Scheduler Persistence and Task Cache

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Long reconstruction pipelines (hundreds of DAG tasks) run for hours and cross worker processes that can crash, be cancelled, or be killed. The platform promises reproducible processing and must survive a host restart: a pipeline interrupted at task 120 of 300 should resume from task 120, not restart. It must also never redo work whose inputs and configuration have not changed, because re-reconstruction is expensive. The scheduler therefore needs durable state and a content-addressed cache, both anchored in the `.spx` project directory and the artifact store.

## Decision

The scheduler persists its state in `project.db` (SQLite, WAL, metadata only) inside the `.spx` project directory, and resumes the DAG after a restart. Task states are `pending`, `running`, `succeeded`, `failed`, `cancelled`, `skipped`, recorded transactionally with the artifact UUIDs of their outputs. Retry policy: tasks with a recoverable error (ADR-014) are retried with a bounded backoff; unrecoverable failures fail the task and cascade per the DAG. Cancellation is a first-class, persisted state: cancelled tasks are never re-run on resume unless explicitly re-queued. The task cache is keyed by a deterministic composite: input artifact hashes (CAS SHA-256), a configuration hash, the producer/algorithm version, and the git commit of the code that produced it. A `deterministic` flag on a task declares whether its output is guaranteed reproducible; deterministic tasks can be satisfied from cache, non-deterministic tasks always re-run. Any change to a cache key component invalidates the entry: if an input artifact, config, producer version, or git commit changes, the cached result is not reused. Cache hits and misses are recorded as structured events (ADR-015) with the cache key, so reproducibility claims can be audited. Scheduler state and cache metadata live in SQLite; artifact bytes live in the CAS store.

## Alternatives

- **In-memory scheduler with no persistence:** rejected — an hours-long pipeline cannot afford restart-from-zero.
- **Persist only the final result:** rejected — loses the ability to resume and the per-task retry bookkeeping.
- **Cache keyed by task name or config only:** rejected — would silently reuse stale outputs when inputs changed, violating reproducibility.

## Consequences

- Positive: resume-after-restart is a differentiator; cache saves hours on iterative workflows; reproducibility is auditable via cache keys and events; retry/cancellation semantics are durable across processes.
- Negative: SQLite writes add overhead per task transition; cache keys must be computed carefully and consistently; invalidation rules add conceptual load.
- Risks and mitigations: risk of stale cache reuse from a weak config hash — mitigated by hashing the effective configuration, not the raw file; risk of SQLite lock contention in a multi-worker host — mitigated by WAL mode and a single writer in Core; risk of non-deterministic tasks corrupting cache — mitigated by the `deterministic` flag rule above.

## References

- `docs/specifications/task-model.md`
- `docs/architecture/storage-model.md`
- `docs/architecture/process-model.md`
- ADR-014 (error handling — recoverable codes drive retry)
- ADR-015 (logging — cache hit/miss events)
