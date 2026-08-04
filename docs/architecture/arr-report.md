# Architecture Readiness Review (ARR) — P0

- **Stage:** 0 (pre-P0-freeze gate)
- **Date:** 2026-08-04
- **Reviewed artifacts:** spatial-rfcs (CONSTITUTION.md, ADR-001..037, RFC-0001), spatial-platform (docs/specifications, docs/architecture, schemas, scripts/CI)
- **Verifier:** Architecture Board review pass
- **Result:** PASS (all 10 sections green; all 7 admission criteria satisfied)

## 1. Domain Consistency

Scene (SceneId UUID) ↔ Observation ↔ GeometryElement ↔ Artifact ↔ Sensor ↔ Project are the only domain entities; no duplicated abstractions. Geometry-first: `GeometryElement` (Point/Triangle/Voxel/Gaussian placeholders) is the single geometry abstraction; "mesh" does not appear as a core type. Scene = three sub-graphs (Observation Graph, Geometry Graph, Relationship Graph) per scene-model.md §3.

## 2. Specification Consistency

Every spec references only ratified entities. Worker protocol message names in worker-protocol.md match worker.proto (`WorkerHello`, `TaskRequest`, `TaskAccepted`, `TaskProgress`, `TaskArtifactProduced`, `TaskCompleted`, `TaskFailed`, `TaskLog`, `TaskCancelled`, `Heartbeat`, `Shutdown`). Deferred layers are explicitly marked `IMPLEMENTATION DEFERRED past M0` (recipe-model, benchmark-framework §6, adaptive-engine, workflow).

## 3. ADR Coverage

37 ADRs cover all decisions; scene-model.md cross-references ADR-005/006/007/008/009/010/016/018/023/024/030/031/032/033/035/036. RFC-0001 ratifies schema contracts and satisfies the protected-path change-control for the P0 baseline commit.

## 4. Dependency Review

Direction is enforced: `core/scene → core/coordinates → core/geometry → core/artifacts → worker`; no reverse edges. Domain types (strict scalars) in `core/coordinates` are the only sink for raw Eigen; business logic never touches Eigen directly. check_arch_debt.py scans 15 protected files and passes.

## 5. Plugin Boundary

Plugins enter only through `adapters/interfaces/**` (capability taxonomy: sparse_reconstruction, dense_stereo, bundle_adjustment, icp, surface_reconstruction, texturing, gaussian_generation, lidar_odometry, loop_closure, gnss_integration — closed enum in worker-capabilities.schema.json, extensible only via RFC). AI capability is `adaptive-engine` prior-only; AI never writes authoritative geometry (verified: adaptive-engine.md:53/73/85/89). No bypass of Core via plugin.

## 6. Serialization Review

- SQLite: UUID as 16-byte BLOB; no binary blobs (ADR-009).
- JSON: UUID as RFC-4122 string; schemas carry `$schema` + version.
- Protobuf: strict wrappers `Uuid`, `TimestampNs`, `DistanceMeters`, `AngleRadians`, `Transform` (`world_from_sensor`).
- Framing: `[u32 LE length][proto bytes]` consistent across worker.proto, worker-protocol.md, IPC ADR-012.
- SQL migration 0001_init.sql present; schema.sql is canonical DDL.

## 7. Coordinate Audit

Every spatial element has an explicit frame answer: camera images → `world_from_sensor`; rig → `rig_from_sensor`; convention set (right-handed, meters, ns, radians, quaternion scalar-last x,y,z,w, column vectors, OpenCV camera, image origin top-left, covariance translation-then-rotation). Raw Eigen forbidden in business logic (check_domain_types.py enforces). No frame-less geometry exists in the model.

## 8. Replaceability

Every algorithm is behind a capability/adapter: COLMAP, OpenMVS, GTSAM, AI inference are replaceable without Core changes (ADR-004/005/006/021). IP (licensed deps) gated by check_dependencies.py allowlist; ADR-005 keeps GTSAM optional to avoid GPL pollution.

## 9. Testability

Modules are isolated by contracts: worker IPC testable with mock adapters (ADR-021); scheduler persistence testable against SQLite WAL in-memory; C++ kernel gtest/gmock; schema validation is the cross-language gate. P1 build CI matrix already fixed.

## 10. Product Review

Platform blueprint defines a 10-year roadmap (VGGT-era AI, Digital Twin epochs, Capture Advisor, Benchmark harness). Public API stability policy (ADR-037) protects commercial SDK investment. No near-term obsolescence: C++20 + Python 3.11 baseline is a platform floor, not a ceiling.

## Admission Criteria

1. ✅ CONSTITUTION.md exists, ratified, with protected surfaces + change control (RFCS repo committed).
2. ✅ ADR-001..037 complete and linked from docs.
3. ✅ Scene is central domain object (ADR-023), Observation Graph canonical (ADR-024).
4. ✅ Geometry-first: GeometryElement is the single geometry abstraction; no mesh-first.
5. ✅ Coordinates fully audited (ADR-007 + coordinate-systems.md + strict types in proto).
6. ✅ All 6 CI gates pass on P0 baseline (constitution via RFC-0001, rfc, schemas, domain-types, arch-debt, dependencies).
7. ✅ First commit is docs/ADR/schemas/CI only — no implementation code in P0.

## Verdict

**READY for P0 freeze.** Baseline is git-immutable. Next: P1 (Project Core + Artifact Store, C++20, CMakePresets + conan install + gtest).
