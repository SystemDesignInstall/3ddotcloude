# ADR-031 — M0 Scope Boundary

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

M0 is the smallest shippable slice of the platform: the kernel, storage, scheduler, and interfaces with which everything else is later built. Without an explicit boundary, scope creep would pull in algorithm backends, GPU rendering, AI checkpoints, and heavy third-party builds before the kernel is stable. The Constitution requires Architecture Debt = 0 in the kernel and forbids "quick prototype then rewrite". The backend registry (THIRD_PARTY.yml) lists COLMAP, OpenMVS, Open3D, GTSAM, Ceres, KISS-ICP, VGGT, LingBot-Map, gsplat, Nerfstudio, FFmpeg, OpenCV, PROJ/GDAL, LASzip, and libE57Format — all status "planned". M0 must not build any of them.

## Decision

This ADR is the M0 scope guardrail. **M0 code** (implemented, tested, shipped): Project Core, Artifact Store (CAS SHA-256, UUID manifests, atomic temp+rename), strict types and coordinates (WorldFromCamera, CameraFromWorld, RigFromSensor, SensorFromRig, TimestampNs, DistanceMeters, AngleRadians), Coordinate Frame Graph, Sensor/SensorRig/Calibration, Observation Graph, Scene Graph (minimal GeometryElement base plus Scene Query API), Scheduler (DAG, retries, cancellation, persisted state, task cache), Worker Protocol (separate processes, Protobuf frames over stdin/stdout, heartbeat, timeout, cancellation, crash detection), a demo Python worker, Python SDK, CLI, mock adapters, importers, and `doctor`. **M0 dependencies**: eigen, protobuf, sqlite3, nlohmann-json, gtest. **M0 non-negotiable exclusions**: no algorithm implementations, no heavy third-party builds, no Qt/Vulkan/CUDA, no AI checkpoints or model downloads, no AI inference. Third-party backends appear only as interface and mock adapters (ADR-021, ADR-034). **Deferred to later milestones — specs, ADRs, schemas, and interfaces only in M0**: Adaptive Engine (ADR-027), Benchmark harness (ADR-029, registry and schemas in P0), Capture Advisor, Workflow Engine (ADR-028), Uncertainty Engine, per-point provenance channels (ADR-025, schema in P0), Quality Engine (ADR-030), Recipe implementation (ADR-026, schema in P0), immutable-scene full semantics (ADR-033, metadata only in M0), Digital Twin processing (ADR-036), Geometry Graph logic (ADR-032, base only in M0), and Relationship Graph.

**Deferred to:** Photogrammetry and later milestones — every item in the exclusion list above.

## Alternatives

- **Thin vertical slice with one real algorithm (COLMAP):** rejected — forces a heavy backend into the kernel and couples M0 to vendor build complexity before contracts are stable.
- **Everything-in-M0:** rejected — violates Architecture Debt = 0 and would make the kernel unreviewable.
- **Interfaces only, no storage/scheduler:** rejected — M0 must prove persistence, process isolation, and reproducibility end to end.

## Consequences

- Positive: kernel is small, reviewable, and debt-free; every later milestone plugs into stable interfaces; mock adapters make the whole chain testable without backends; risk of vendor lock-in and build hell is deferred.
- Negative: M0 cannot produce a real reconstruction; demo value comes from mock adapters and importers; users see no production geometry until Photogrammetry.
- Risks and mitigations: risk of redefining "in scope" mid-M0 — any proposed addition must be judged against this ADR and Constitution §4; risk that deferred interfaces get designed later and break M0 contracts — mitigated by ratifying schemas and interfaces in P0 as listed above; risk of M0 demo being underwhelming — mitigated by a compelling demo worker and importers.

## References

- `docs/specifications/scene-model.md`
- `docs/specifications/reconstruction-pipeline.md`
- `docs/specifications/task-model.md`
- ADR-008/ADR-009/ADR-010 (project storage, metadata separation, artifact store), ADR-011/ADR-012 (worker isolation and IPC), ADR-020 (scheduler), ADR-021 (mock adapters), ADR-025/ADR-026/ADR-029/ADR-034/ADR-035 (P0 schemas and interfaces)
