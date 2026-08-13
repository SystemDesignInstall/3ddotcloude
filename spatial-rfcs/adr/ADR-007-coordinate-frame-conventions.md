# ADR-007 — Coordinate-frame conventions

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Mixed conventions are the leading source of 3D reconstruction bugs: handedness, units, quaternion order, camera models, and pose composition. Every component — C++ kernel, Python SDK, adapters, third-party backends, rendering — must agree on one set of conventions, enforced by the type system.

## Decision

All geometry uses exactly these conventions:

- Right-handed coordinate systems; units are meters (DistanceMeters); time is nanoseconds (TimestampNs).
- Column-vector convention: p_world = R * p_sensor + t, written world_from_sensor; SE(3) = (R, t).
- Quaternions are scalar-last (x, y, z, w).
- Angles are radians internally (AngleRadians); rotations use strict types WorldFromCamera, CameraFromWorld, RigFromSensor, SensorFromRig.
- Camera convention is OpenCV: +x right, +y down, +z forward; image origin is top-left.
- Covariance ordering is translation-then-rotation.
- Raw Eigen matrices and untyped doubles are forbidden in business logic; they are allowed only in core/geometry math internals and adapters, converted at the boundary through strict domain types.
- OpenGL/Vulkan conversion (y-flip, handedness changes) is handled at the rendering boundary, never in Core.

## Alternatives

- Per-component conventions with conversion at integration time: rejected. Proven source of silent drift and bugs.
- Typedef-only enforcement: rejected. Typedefs do not prevent misuse of raw double/float values.
- Left-handed or z-up world: rejected. Right-handed with the OpenCV camera convention matches the reference pipelines and sensor models.

## Consequences

- Positive: compositional correctness (p_world = R * p_sensor + t) verified by unit and property-based tests; strict types make misuse fail at compile time; one set of conventions for all adapters.
- Negative: adapters must convert third-party conventions (COLMAP, GTSAM, Open3D, OpenCV, GLTF) at the boundary; some friction with rendering stacks that assume OpenGL conventions.
- Risks and mitigations: the check-domain-types CI gate rejects raw Eigen in business logic; property-based transform round-trip tests and coordinate-conversion tests in the unit suite; conversions documented in docs/architecture/coordinate-systems.md.

## References

- docs/architecture/coordinate-systems.md
- docs/specifications/sensor-model.md
- ADR-004 (COLMAP as canonical SfM backend)
- ADR-005 (GTSAM as unified sensor factor graph)
