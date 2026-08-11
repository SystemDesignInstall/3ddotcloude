# Project Context Summary

- **Status:** ratified (P0, maintained by the Architecture Board)
- **Purpose:** a living summary of the Spatial Platform — architecture, governance, milestones, and current development focus. Source of truth for milestone naming and priorities.

## 1. Mission

Spatial Platform is a scene-centric, reproducible 3D reconstruction platform (RealityCapture/Metashape class) built as a permanent, versioned, provenance-rich spatial data model with an isolated, capability-selected compute engine.

## 2. Repositories

- `spatial-rfcs` — governance: CONSTITUTION.md, ADRs (ADR-001..039), RFCs (RFC-0001..0005).
- `spatial-platform` — implementation: C++20 kernel (`core/`), execution engine (`engine/`), adapters, CLI, Python SDK, schemas, tests.

## 3. Governance model

- CONSTITUTION.md §2 freezes protected surfaces; changes require a ratified RFC cited in the commit/PR body (`check_constitution.py`).
- 16 Architecture Principles; Architecture Debt = 0 in `core/**`, `engine/**`, `schemas/**`.
- RFC chain: RFC-0001 (Schema Contracts) → RFC-0002 (Permanent Spatial Data Model) → RFC-0003 (Processing Engine) → RFC-0005 (Accuracy & Quality Assurance) → RFC-0004 (future: Plugin & Worker Ecosystem).

## 4. Milestone naming

- ADR-031 defines **M0** as the smallest shippable slice (kernel, storage, scheduler, worker protocol, CLI, mocks).
- `docs/agent-tasks/README.md` (ratified P0) defines domain lanes P0→P4; the Core Platform lane owns `engine/**`, `core/project|storage|artifacts|errors`, `cli`. Lane labels are team-lane build phases and are **independent** of the product milestones below.
- The **P1** Core Platform lane (P1.1–P1.5: execution core, scheduler, workers, mock pipeline, CLI, quality engine) is complete. The current product milestone is **P2 — Photogrammetry Core** (P2.1–P2.5, see §15, PPS-0001). RFC-0002 implementation stages used the historical P2a..P2d labels.

## 5. Ratified foundations

- RFC-0001: initial schema contracts (worker.proto, errors.proto, scene.proto, JSON schemas, SQLite DDL).
- RFC-0002: permanent spatial data model — SceneVersion/ChangeLog, CaptureSession, Device/SensorRig/Sensor, Observation Graph + relations, Geometry identity/provenance, capability versioning, time model.
- RFC-0003: Processing Engine & Execution Architecture — Task model, TaskGraph DAG, six-state lifecycle, retry/cancel, cache (ADR-020), worker protocol, ExecutionRecord, Pipeline/Workflow surfaces.
- RFC-0005: Accuracy & Quality Assurance — Quality Engine as first-class (ADR-030), `quality-report.schema.json`, TaskRequest `pipeline_hash`, CLI `spatial report`.
- ADR-038: Processing Engine boundary — owns runtime execution, never algorithms/storage/GPU/UI.

## 6. Implemented to date

- P1 (core): project core + artifact store (CAS SHA-256, SQLite WAL, gtest 40/40) — commit `c36b1f4`.
- P2a (RFC-0002): coordinates/geometry/sensor-time foundation — commit `cd12a28` (72/72 tests).
- P1 engine (RFC-0003): engine core + workers + mock pipeline + CLI — see current focus (§15).
- P1.5 (RFC-0005): Quality Engine — schema + spec + quality report + TaskRequest `pipeline_hash` + CLI `spatial report` — commits `bcbd033`, `513bc99` (139/139 Debug + Release).

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
Milestone:     P2 — Photogrammetry Core (RFC-0002, ADR-004; PPS-0001)
Goal:          Turn the frozen data model into the platform's first real
               photogrammetry capability: images in, sparse scene out,
               with full provenance and quality.
Specification: PPS-0001 — docs/PPS-0001-platform-principles.md
```

Stage map (PPS-0001 implementation plan):

- **P2.1** Image Import — `ImageArtifact` (metadata/EXIF/thumbnail/hash), `importers/images`, `ImageObservation` records.
- **P2.2** Camera Model — canonical taxonomy in `calibration.schema.json` (`fov`, `brown_conrady` alias), `core/calibration`, `core/sensors`.
- **P2.3** Feature Extraction — `FeatureArtifact`, capability `FeatureExtraction`, COLMAP-first adapter behind `adapters/interfaces/**`. Status: mock `feature_extract` runner (RFC-0007 C3) and the read-boundary bridge (`SceneQuery::ArtifactHash` / `SceneVersion` reads, RFC-0007 C4) complete; CLI/session wiring (`spatial run feature-extraction --session`) pending as C5.
- **P2.4** Feature Matching — `MatchArtifact`, capability `FeatureMatching`.
- **P2.5** Bundle Adjustment — `SparseModel` artifact, capabilities `SparseReconstruction` + `BundleAdjustment`, COLMAP default (ADR-004).

Out of P2: Dense/Mesh/Texture/Gaussian, adaptive engine, distributed workers, AI priors (post-M0 per ADR-031; deferred artifact types reserved in PPS-0001 §11).
