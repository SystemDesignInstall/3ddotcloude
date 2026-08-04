# Worker Protocol Specification

Status: Draft
References: ADR-011 (adapter process isolation), ADR-012 (worker lifecycle and reliability), ADR-014 (structured task protocol)

This document defines the IPC protocol between the Spatial Platform scheduler (engine) and algorithm workers. A worker is a child process running an adapter's algorithm. The protocol is stable and versioned; M0 (Section 8) is the reference implementation.

## 1. Transport

- The engine spawns the worker as a child process. Communication happens over the child's **stdin (engine → worker)** and **stdout (worker → engine)**.
- Every message is framed as `[u32 little-endian length][protobuf bytes]`. The length is the byte count of the serialized message, not including the 4-byte length prefix. A message must never be split or coalesced at the frame boundary.
- The worker's **stderr is captured separately** by the engine and recorded as `TaskLog` entries in the project `logs/` directory (see `project-format.md` Section 6). Stderr is for diagnostics only and is never a channel of protocol data.
- All message bodies are protocol-buffer encoded. One `.proto` file defines the whole protocol; message names below map to protobuf message names.

A concrete frame carrying a 4-byte body is `04 00 00 00` followed by the four body bytes. An implementation must read exactly `length` bytes after the prefix before attempting to parse; a malformed frame (prefix claims more bytes than the stream contains) is a protocol error that terminates the worker.

The wire format is symmetric: both sides must be prepared to read any valid message type at any time (e.g. the engine may send `TaskCancelled` while the worker is mid-task), and message dispatch is by the protobuf oneof `tag`.

## 2. Messages

Exact names, purpose, and key fields:

| Message | Direction | Purpose | Key fields |
| --- | --- | --- | --- |
| `WorkerHello` | worker → engine | First message; announces the worker | `protocol_version`, `worker_id`, `capabilities` |
| `WorkerCapabilities` | worker → engine | Full capability declaration | `capability_list`, `resource_profile`, `max_concurrency` |
| `TaskRequest` | engine → worker | Dispatch a task | `task_id`, `spec`, `workspace` |
| `TaskAccepted` | worker → engine | Task entered execution | `task_id` |
| `TaskProgress` | worker → engine | Progress reporting | `task_id`, `percent`, `substage`, `message` |
| `TaskLog` | worker → engine | Structured log line | `task_id`, `level`, `message`, `context` |
| `TaskArtifactProduced` | worker → engine | Artifact handoff | `task_id`, `artifact_ref`, `metadata` |
| `TaskCompleted` | worker → engine | Successful finish | `task_id`, `outputs[]` |
| `TaskFailed` | worker → engine | Failure with structured error | `task_id`, `error { code, message, recoverable }` |
| `TaskCancelled` | engine → worker | Cooperative cancel request | `task_id`, `reason` |
| `Heartbeat` | worker → engine | Liveness signal | `worker_id`, `timestamp`, `rss` |
| `Shutdown` | engine → worker | Graceful shutdown request | (none; request is by presence) |

`spec` is the canonical task specification; `workspace` is the deterministic temp directory for the task (see `project-format.md` Section 7). `artifact_ref` points at a produced artifact in the store.

The schema is a single protobuf message wrapping everything:

```proto
message WorkerMessage {
  oneof payload {
    WorkerHello hello = 1;
    WorkerCapabilities capabilities = 2;
    TaskRequest task_request = 3;
    TaskAccepted task_accepted = 4;
    TaskProgress task_progress = 5;
    TaskLog task_log = 6;
    TaskArtifactProduced artifact_produced = 7;
    TaskCompleted task_completed = 8;
    TaskFailed task_failed = 9;
    TaskCancelled task_cancelled = 10;
    Heartbeat heartbeat = 11;
    Shutdown shutdown = 12;
  }
}
```

Ordering rules: `WorkerHello` is always the first message; within a task, `TaskAccepted` precedes any `TaskProgress`/`TaskLog`/`TaskArtifactProduced`, and exactly one of `TaskCompleted`/`TaskFailed` terminates the task's message stream. A worker that violates an ordering rule is terminated by the engine as a protocol error.

## 3. Versioning

- The worker announces `protocol_version` in `WorkerHello`. The engine rejects workers whose version is incompatible and terminates them with a logged reason.
- After the handshake the worker sends `WorkerCapabilities`; the engine only dispatches tasks whose capability requirements the worker has declared.
- Unknown message types are ignored and logged by the receiving side. A worker must never crash on an unrecognized frame; an engine must never treat an unknown worker message as fatal. Message fields are additive: a receiver uses the fields it knows and ignores the rest.

## 4. Lifecycle

The state machine is linear:

```
handshake (WorkerHello + WorkerCapabilities)
  → idle
    → task (TaskRequest → TaskAccepted → [TaskProgress | TaskLog | TaskArtifactProduced]* → TaskCompleted | TaskFailed)
    → idle
  → shutting_down (Shutdown)
  → exit
```

- A worker processes one task at a time up to `max_concurrency`; the engine must not dispatch more concurrent tasks than the worker's declared limit.
- After `TaskCompleted` or `TaskFailed` the worker returns to `idle` and may be given another task. The engine never reuses a crashed worker; it is replaced with a fresh process.
- On `Shutdown` the worker finishes or cancels in-flight tasks, sends a final `Heartbeat`, and exits with code 0.

Edge cases: a worker that exits after sending `TaskCompleted` (rather than returning to `idle`) is treated as idle; a worker that sends `TaskCompleted` for a task it was never given is a protocol error; a `WorkerHello` after shutdown is ignored and the process is terminated.

## 5. Reliability

- **Heartbeat timeout:** the default is **5 seconds** with no heartbeat. A worker that misses the timeout is declared dead.
- **Crash detection:** the engine detects process exit (nonzero or unexpected), a missing heartbeat, or a closed stdout. In all cases the task is marked `interrupted`.
- **Requeue:** an interrupted task is requeued according to the retry policy: up to 2 retries by default, `recoverable` failures retried with exponential backoff, non-recoverable failures never retried. Retried tasks are re-run from scratch with a fresh deterministic workspace.
- **Deterministic workspace:** each task gets `temp/<job_id>/<task_id>`; the workspace is created before dispatch and cleaned up after completion, cancellation, or crash.
- **Safe cleanup:** the engine removes orphaned workspaces at project close; a crash never leaves a workspace that a later retry could confuse.

## 6. Cancellation

- Cancellation is cooperative: the engine sends `TaskCancelled` with a `reason`; the worker should stop at the next safe checkpoint and reply with `TaskCancelled`-related `TaskProgress` (or simply exit to `idle` after emitting a `TaskFailed`-style structured error is **not** used — cancellation is reported via `TaskProgress`/`TaskCompleted` semantics defined by the task kind).
- If the worker does not acknowledge cancellation within the timeout, the engine escalates: it sends `Shutdown`, and if the process still does not exit, it terminates the process.

## 7. Error model

Errors cross the process boundary only as structured `error` values: `code` (stable machine-readable string), `message` (human-readable), `recoverable` (bool). Exceptions, stack traces, and raw stderr text never cross as protocol data. The `recoverable` flag is the only input the scheduler's retry policy uses to decide whether a failed task may be retried.

## 8. M0: demo Python worker

The M0 worker is a Python script implementing the protocol end-to-end:

1. Read framed messages from stdin; write framed `WorkerHello` then `WorkerCapabilities` on startup.
2. Declare one capability and `max_concurrency = 1`.
3. On `TaskRequest`: send `TaskAccepted`, then a loop of `TaskProgress` 0 → 100%, then write a payload file into the task `workspace`, send `TaskArtifactProduced`, and finally `TaskCompleted` with the artifact as the sole output.
4. Emit `Heartbeat` at 1-second intervals; respond to `Shutdown` by exiting cleanly.

M0's purpose is to validate framing, the state machine, heartbeat detection, and artifact handoff against the real engine before any production adapter exists.
