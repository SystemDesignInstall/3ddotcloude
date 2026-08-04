# ADR-018 — Strict Scalar and Transform Types

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

The dominant failure class in photogrammetry software is unit and frame confusion: meters interpreted as millimeters, quaternion order mistakes, a sensor transform applied in the wrong direction, timestamps in different epochs. Spatial Platform commits to explicit coordinate conventions (right-handed, meters, nanoseconds, quaternion (x,y,z,w) scalar-last, column vectors, `p_world = R*p_sensor + t`, `world_from_sensor`, OpenCV camera convention with +x right/+y down/+z forward, image origin top-left, radians internal, SE(3)=(R,t), covariance translation-then-rotation). Conventions that live only in documentation will be violated; they must live in the type system so the compiler rejects violations. These types are constitution-protected surfaces.

## Decision

Business logic uses strict domain types and never raw Eigen matrices. The fixed vocabulary: `WorldFromCamera`, `CameraFromWorld`, `RigFromSensor`, `SensorFromRig`, `TimestampNs`, `DistanceMeters`, `AngleRadians`. Transforms are named by source and destination frame so composition and inversion are explicit and verified by `check-domain-types` in CI. Raw Eigen is permitted only inside `core/geometry/**` internals (where the arithmetic actually happens) and inside adapters, behind the adapter boundary (ADR-021); it never appears in public interfaces, scene objects, schemas, or the SDK. Conversion between strict types and raw values happens only at explicit IO/export boundaries and at the Protobuf IPC edge, where the wire format carries units and frames explicitly. Covariance is stored in the translation-then-rotation ordering defined by the conventions. Enforcement is automated: a CI gate (`check-domain-types`) rejects raw Eigen types leaking into domain code, reject arithmetic on mixed unit types, and verifies that every transform constructor documents its frame pair. The typed hierarchy is the mechanism behind coordinate validation, and the `CoordinateError` family (ADR-014) reports frame mismatches with stable codes.

## Alternatives

- **Documentation-only conventions:** rejected — already known to fail at scale; no compiler enforcement.
- **Custom `Units`/`Quantity` library with full dimensional analysis:** rejected as over-engineering; the fixed vocabulary covers the platform's actual units and frames without a general type system.
- **Raw Eigen everywhere:** rejected — defeats the purpose and leaks frames into every layer.

## Consequences

- Positive: unit and frame errors become compile-time or CI-time failures; code reads self-documentingly (`WorldFromCamera`, `DistanceMeters`); interoperability with backends stays explicit at boundary conversions; convention errors surface in tests, not in customer reconstructions.
- Negative: boilerplate at conversion boundaries; a longer list of types to design and maintain; internal Eigen code must be disciplined about not leaking.
- Risks and mitigations: risk of developers bypassing strict types for speed — mitigated by the constitution protecting `core/coordinates/**` and `core/geometry/**`, and by `check-domain-types` in CI; risk of conversion bugs at boundaries — mitigated by golden/property tests for every transform pair (ADR-016).

## References

- `docs/specifications/geometry-model.md`
- `docs/specifications/error-model.md`
- ADR-019 (Eigen adoption — the underlying linear algebra behind the strict types)
- ADR-016 (testing strategy — property/golden tests for transforms)
- ADR-021 (mock adapters — raw Eigen allowed only behind adapter boundaries)
