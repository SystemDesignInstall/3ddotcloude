# Project Context Summary

- **Status:** ratified (P0, maintained by the Architecture Board)
- **Purpose:** a living summary of the Spatial Platform — architecture, governance, milestones, and current development focus. Source of truth for milestone naming and priorities.

## 1. Mission

Spatial Platform is a scene-centric, reproducible 3D reconstruction platform (RealityCapture/Metashape class) built as a permanent, versioned, provenance-rich spatial data model with an isolated, capability-selected compute engine.

## 2. Repositories

- `spatial-rfcs` — governance: CONSTITUTION.md, ADRs (ADR-001..038), RFCs (RFC-0001..0003).
- `spatial-platform` — implementation: C++20 kernel (`core/`), execution engine (`engine/`), adapters, CLI, Python SDK, schemas, tests.

## 3. Governance model

- CONSTITUTION.md §2 freezes protected surfaces; changes require a ratified RFC cited in the commit/PR body (`check_constitution.py`).
- 16 Architecture Principles; Architecture Debt = 0 in `core/**`, `engine/**`, `schemas/**`.
- RFC chain: RFC-0001 (Schema Contracts) → RFC-0002 (Permanent Spatial Data Model) → RFC-0003 (Processing Engine) → RFC-0004 (future: Plugin & Worker Ecosystem).

## 4. Milestone naming

- ADR-031 defines **M0** as the smallest shippable slice (kernel, storage, scheduler, worker protocol, CLI, mocks).
- `docs/agent-tasks/README.md` (ratified P0) defines domain lanes P0→P4; the Core Platform lane owns `engine/**`, `core/project|storage|artifacts|errors`, `cli`. P1 = scheduler + workers + CLI.
- Implementation stages of RFC-0002 use P2a..P2d. The engine milestone is the **P1** Core Platform lane (P1.1–P1.4).

## 5. Ratified foundations

- RFC-0001: initial schema contracts (worker.proto, errors.proto, scene.proto, JSON schemas, SQLite DDL).
- RFC-0002: permanent spatial data model — SceneVersion/ChangeLog, CaptureSession, Device/SensorRig/Sensor, Observation Graph + relations, Geometry identity/provenance, capability versioning, time model.
- RFC-0003: Processing Engine & Execution Architecture — Task model, TaskGraph DAG, six-state lifecycle, retry/cancel, cache (ADR-020), worker protocol, ExecutionRecord, Pipeline/Workflow surfaces.
- ADR-038: Processing Engine boundary — owns runtime execution, never algorithms/storage/GPU/UI.

## 6. Implemented to date

- P1 (core): project core + artifact store (CAS SHA-256, SQLite WAL, gtest 40/40) — commit `c36b1f4`.
- P2a (RFC-0002): coordinates/geometry/sensor-time foundation — commit `cd12a28` (72/72 tests).
- P1 engine (RFC-0003): engine core + workers + mock pipeline + CLI — see current focus (§15).

## 7. Key technical invariants

- SQLite stores metadata/indices only; payloads live in the CAS (ADR-008/009/010).
- One SQLite writer; exactly one allocator (the scheduler).
- Workers are isolated child processes over Protobuf IPC (ADR-011/012).
- Task cache key: input content hashes + config hash + producer version + engine git commit (ADR-020).

## 8. Gates

`check_rfc`, `check_constitution --rfc RFC-NNNN`, `check_domain_types`, `check_schemas`, `check_dependencies`, `check_arch_debt`; Debug + Release ctest 100%.

## 9–14. (Reserved)

Sections 9–14 are reserved for domain details: scene model, sensor model, geometry model, plugin API, benchmarks, and testing strategy.

## 15. Current Development Focus

```
Milestone:     P1 — Processing Engine (Core Platform lane; RFC-0003, ADR-038)

Goal:          Transform the Spatial Platform from a persistent storage system
               into an executable spatial computing platform.

Primary RFC:   RFC-0003 Processing Engine & Execution Architecture

First deliverable — end-to-end execution:

Artifact
    ↓
Pipeline (model) → Workflow (model) → Task DAG
    ↓
Scheduler
    ↓
Worker (ProcessExecutor / InProcessExecutor)
    ↓
Artifact
```

Stage map (RFC-0003 Implementation Plan):

- **P1.1** Execution Core — `engine/task`, `engine/resources`, `engine/execution`, migration `0003_scheduler.sql`, SCHED_*/WORKER_* codes.
- **P1.2** Scheduler Runtime — `engine/scheduler`, `engine/cache`.
- **P1.3** Workers — `engine/workers` (IPC + demo Python worker), integration tests.
- **P1.4** First Pipeline — mock photogrammetry pipeline, CLI `spatial run`/status.

Out of P1: algorithms, production local executor, GPU management, distributed workers, recipe/workflow execution, AI workers (post-M0 per ADR-031).
