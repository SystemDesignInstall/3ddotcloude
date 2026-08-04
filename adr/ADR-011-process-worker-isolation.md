# ADR-011 — Process worker isolation

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

External components (COLMAP, OpenMVS, Open3D, AI models, SLAM, exporters) are C/C++ or Python binaries with different lifecycles and failure modes. A crash or memory blowup in a backend must not take down Core or corrupt the user's project. M0 includes a demo Python worker (ADR-031).

## Decision

Every external component runs in a separate worker process. Isolation provides crash isolation, per-process resource limits, and restart capability. Each worker gets a deterministic temp workspace under temp/ (ADR-008), with stdout/stderr captured for logging. Core supervises workers via heartbeat and timeout, and cancellation is delivered via a protocol message (ADR-012). Safe cleanup removes temp workspaces and reaps processes on completion or crash. The scheduler tracks worker state as part of task state (pending/running/succeeded/failed/cancelled/skipped) and persists enough state to resume after an application restart.

## Alternatives

- In-process backend calls: rejected. A backend crash or OOM kills Core; no resource limits; no restart.
- Thread-based concurrency: rejected. Python and native third-party state make isolation impossible.
- Microservice per backend over a network: rejected. Overkill for M0; local process boundaries are simpler and lower latency.

## Consequences

- Positive: crash and memory isolation; clean restart; resource accounting; deterministic workspaces; captured logs for diagnosis.
- Negative: IPC overhead and marshaling cost; process lifecycle management complexity; zombie and reaping handling on both Windows and Linux.
- Risks and mitigations: worker-protocol tests cover crash detection, timeout, cancellation, and resume; fault-injection tests (lost worker, kill during write, disk full); structured logs carry worker_id for diagnosis.

## References

- docs/specifications/worker-protocol.md
- docs/specifications/task-model.md
- ADR-012 (IPC protocol)
- ADR-008 (Project storage format)
