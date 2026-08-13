# ADR-015 — Logging and Provenance

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform processes long-running pipelines across the main process, worker processes, and the scheduler. Debugging a reconstruction failure requires correlating events across a job: which project, which job, which task, which worker. The platform also promises reproducible processing and provenance for artifacts (CAS SHA-256 store, UUID manifests, GC, immutable artifacts). Logs are needed for observability, but provenance must never live only in logs: provenance must survive GC, log rotation, and process crashes. Logs must also never contain secrets or PII, because they are shipped to operators and CI.

## Decision

Logging is JSON-lines to `logs/` in the `.spx` project directory. Each event is a single JSON object with the fields: `timestamp`, `severity`, `component`, `project_id`, `job_id`, `task_id`, `worker_id`, `event`, `message`, `context`, `error_code`, `stack`. Correlation ids (`project_id`, `job_id`, `task_id`, `worker_id`) are propagated from Core through the worker protocol so that a single chain of events across processes can be reassembled. Structured fields are preferred over free-form message text; `context` is a bounded map for arbitrary structured data. Errors log their stable `error_code` (ADR-014) and, at most one level, their `stack`. Provenance is data, not logs: artifact provenance (who produced an artifact, from which inputs, with which version and git commit) is recorded in the artifact store manifests and the scene graph, never reconstructed from log files. Logging policy: no secrets, no PII, no full file paths that expose user identities, no image content; identifiers are opaque UUIDs. The pipeline of provenance flows from observation ingestion through algorithms to artifact manifests, and logs reference those manifests by UUID so logs and provenance can be joined without duplicating provenance in log lines.

## Alternatives

- **Text logs with timestamps only:** rejected — unparseable across processes, cannot join with provenance, cannot feed structured dashboards.
- **Provenance stored in logs:** rejected — lost on rotation/GC, unsearchable, violates reproducibility guarantees.
- **A dedicated logging service:** rejected as premature; JSON-lines files satisfy M0 while remaining compatible with later ingestion.

## Consequences

- Positive: cross-process correlation via propagated ids; machine-parseable events drive tooling and dashboards; provenance survives independently of logs; secrets/PII policy enforceable by review and CI checks; error correlation into the typed error model.
- Negative: JSON serialization overhead per event; more discipline required in every component; bounded `context` requires governance to avoid bloat.
- Risks and mitigations: risk of PII leaking into `context` — mitigated by review guidance and a CI scan; risk of provenance and logs diverging — mitigated by joining on artifact UUIDs and treating provenance manifests as the source of truth.

## References

- `docs/specifications/error-model.md`
- `docs/architecture/storage-model.md`
- ADR-014 (error handling — stable codes in log events)
- ADR-023 (scene as central domain object — scene-level provenance)
- ADR-024 (observation graph — provenance at ingestion)
