# ADR-014 — Error Handling

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Spatial Platform is a multi-process system: the Core runs in the main process while algorithms run in separate worker processes communicating over Protobuf IPC, with a persistent scheduler, an artifact store, and a typed geometry/scene layer. A bare exception type and message string cannot survive these boundaries, and cannot drive the UI's "recoverable? suggested action?" behavior. Errors must be machine-readable so the SDK/CLI can react, the scheduler can classify retryability, and operators can correlate failures with logs. The error contract is itself part of the public API and must evolve only through RFC.

## Decision

All errors derive from a single typed hierarchy rooted at `ProjectError` with subtypes `StorageError`, `SchemaError`, `CoordinateError`, `CalibrationError`, `ImportError`, `ArtifactError`, `SchedulerError`, `WorkerError`, `AdapterError`, `ValidationError`. Every error carries a stable, never-reused numeric code, a human message, structured context (key/value), a `recoverable` flag, a suggested action, and a chained cause. Error codes are declared in `schemas/**` and are part of the public contract; adding or changing a code requires a ratified RFC. Exceptions never cross process boundaries: at every IPC boundary, including the worker protocol and Protobuf frames, exceptions are translated into structured error payloads with the stable code, message, and context. No C++ exceptions are thrown across the scheduler/worker interface. Internal Core boundaries are `noexcept` where contractually guaranteed (e.g. observers, cancellation checks); exceptions remain the mechanism for genuine programming errors within a process. The error hierarchy is used consistently by validation, adapters, storage, and the scheduler so that retry and resume logic can rely on `recoverable` and code rather than message text.

## Alternatives

- **Flat error codes only, no hierarchy:** rejected because callers lose the ability to handle whole families (e.g. any `StorageError`) uniformly.
- **Free-form exceptions with string messages:** rejected because they cannot cross IPC, cannot drive retry classification, and have no stable contract.
- **Error codes returned by value everywhere:** rejected as laborious for deep internal layers; exceptions are used within a process and translated only at boundaries.

## Consequences

- Positive: stable, documented error vocabulary for SDK and CLI; the scheduler can retry only recoverable failures; logs and worker crashes correlate through stable codes; the typed hierarchy gives precise UI suggestions and user-facing actions; schema validation can check codes at CI time.
- Negative: translation code at every boundary adds boilerplate; the hierarchy is more to design and keep consistent; misuse of `noexcept` can silently mask failures.
- Risks and mitigations: risk of code duplication or drift between languages — mitigated by generating code lists from `schemas/**`; risk of swallowing exceptions at boundaries — mitigated by mandatory cause chaining and tests that assert cause preservation.

## References

- `docs/specifications/error-model.md`
- `docs/architecture/error-model.md`
- `docs/architecture/process-model.md`
- ADR-015 (logging and provenance — error codes correlated into log events)
- ADR-020 (scheduler persistence — retry on recoverable codes)
