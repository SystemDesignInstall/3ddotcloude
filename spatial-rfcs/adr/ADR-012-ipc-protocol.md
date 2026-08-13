# ADR-012 — IPC protocol

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Core supervises worker processes and must exchange structured, versioned messages. Framing must be simple, robust to partial reads and writes, and support binary payloads. No shared-memory assumption holds across Windows and Linux; child-process stdin/stdout pipes are the universal transport.

## Decision

IPC uses Protobuf messages framed as [u32 little-endian length][protobuf bytes] over the worker child process's stdin/stdout. Messages include: WorkerHello, WorkerCapabilities, TaskRequest, TaskAccepted, TaskProgress, TaskLog, TaskArtifactProduced, TaskCompleted, TaskFailed, TaskCancelled, Heartbeat, and Shutdown. The protocol is versioned; on startup the worker sends WorkerHello with its protocol version. Capability negotiation uses WorkerCapabilities with the capability taxonomy (SparseReconstruction, DenseStereo, BundleAdjustment, ICP, SurfaceReconstruction, Texturing, GaussianGeneration, LidarOdometry, LoopClosure, GnssIntegration, extensible). Errors use structured error codes from the typed error model (WorkerError, AdapterError, ValidationError) with stable codes, human-readable messages, technical context, a recoverable flag, a suggested action, and a chained cause. Process crash detection is based on EOF/pipe closure combined with heartbeat timeout; cancellation is delivered as a protocol message and acknowledged.

## Alternatives

- JSON-lines over pipes: rejected. No binary payload support, slower, and no schema validation.
- Shared memory plus named pipes: rejected. Platform-specific, complex lifecycle, and harder crash detection.
- REST/HTTP over localhost: rejected. Higher overhead, port conflicts, and no natural binary framing.

## Consequences

- Positive: versioned, schema-validated messages; binary payloads handled by artifact references (hashes, not embedded bytes); platform-uniform transport; clean crash detection; testable framing.
- Negative: framing and marshaling code to maintain; large messages need chunking or artifact indirection; protobuf schema changes require migration discipline.
- Risks and mitigations: serialization round-trip tests; worker-protocol tests in the integration suite; schema validation in CI; protocol-version negotiation on WorkerHello rejects mismatched workers.

## References

- docs/specifications/worker-protocol.md
- docs/specifications/task-model.md
- ADR-011 (Process worker isolation)
- ADR-010 (Content-addressed artifact store)
