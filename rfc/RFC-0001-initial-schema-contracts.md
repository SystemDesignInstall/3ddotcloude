# RFC-0001 — Initial Schema Contracts

- **Status:** ratified
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none
- **Protected surfaces touched:** Artifact Format (`schemas/**`), Coordinate System (`core/coordinates/**`), Scene (`core/scene/**`), Observation (`core/scene/observation_graph/**`), GeometryElement (`core/scene/geometry/**`), UUID, Plugin API, Capability API

## Summary

Ratifies the initial versions of all Constitution-protected contracts of the Spatial Platform, created as part of the P0 Architecture Freeze. This RFC establishes the baseline that future changes are measured against.

## Motivation

The P0 milestone defines the architecture before any implementation code exists. The schema contracts (protobuf, JSON Schema, SQL DDL) are the cross-language interface between the C++ kernel and the Python SDK. They must be ratified as the change-control baseline so that any later modification follows the RFC process (CONSTITUTION.md §2).

## Design

Ratified baseline (2026-08-04, git-immutable):

- `spatial-platform/schemas/protobuf/errors.proto` — ErrorInfo + ErrorDomain (ADR-014).
- `spatial-platform/schemas/protobuf/worker.proto` — worker IPC protocol messages, framing `[u32 LE length][proto]` (ADR-011, ADR-012).
- `spatial-platform/schemas/protobuf/scene.proto` — scene entity envelope: Uuid, TimestampNs, DistanceMeters, AngleRadians, Transform (world_from_sensor), CoordinateFrame, Scene, Sensor, SensorRig, Frame, Pose, Observation, GeometryElement, ControlPoint, QualityReportRef (ADR-007, ADR-023, ADR-024, ADR-032).
- `spatial-platform/schemas/json/*` — project, artifact-manifest, scene, geometry-element, calibration, pipeline-config, worker-capabilities (active), recipe, workflow, benchmark-report (deferred, `IMPLEMENTATION DEFERRED past M0`).
- `spatial-platform/schemas/database/schema.sql` + `migrations/0001_init.sql` — SQLite metadata DDL (ADR-008, ADR-009).

Conventions frozen by this RFC: UUID as 16-byte BLOB in SQLite / RFC-4122 string in JSON; timestamps as nanoseconds; meters; radians; quaternion scalar-last (x,y,z,w); column vectors; `p_world = R * p_sensor + t`; `world_from_sensor`; OpenCV camera convention; image origin top-left; no large binary data in SQLite; capability taxonomy enumerated in `worker-capabilities.schema.json` and `recipe.schema.json`.

## Compatibility

Baseline — no prior versions exist. All future changes to these files require a new RFC (superseding this one where applicable).

## Alternatives

- No RFC, write contracts directly — rejected: violates the Constitution change-control rule and the "public contracts evolve only through RFC" principle (Principle 10).
- Defer schema ratification to P1 — rejected: schemas are the contract that P1/P2 implement against; ratifying now freezes the interfaces before implementation.

## Open Questions

None.

## Impact

- Sets the change-control baseline for `schemas/**`.
- `check_constitution.py` will pass for P0 commits referencing `RFC-0001`.
- All P1+ implementation must conform to these contracts.
