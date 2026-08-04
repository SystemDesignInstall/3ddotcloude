# Coordinate Systems — Conventions Reference

- **Status:** ratified (P0)
- **Source of truth:** ADR-007 (coordinate-frame conventions), ADR-018 (strict scalar and transform types)
- **Protected surface:** `core/coordinates/**`, `core/geometry/**` (Constitution §2)

This document is THE coordinate conventions reference for the Spatial Platform. It mirrors ADR-007 exactly and is normative for every component: C++ kernel, Python SDK, adapters, third-party backends, and rendering. Any code that disagrees with this document is wrong, regardless of whether it passes tests.

## 1. Right-handed frame

- All coordinate systems are **right-handed**. There is exactly one frame convention; a frame that cannot be expressed as a right-handed system is converted at the boundary or rejected.
- `+x` right, `+y` down, `+z` forward for **cameras** (OpenCV convention, §7). World frames follow the same right-handed rule with the handedness fixed by the frame's definition, never by per-component whim.

## 2. Units

| Quantity | Domain type | SI unit |
|---|---|---|
| Distance | `DistanceMeters` | meters |
| Time | `TimestampNs` | nanoseconds since an explicit epoch (`TimeDomain`) |
| Angle | `AngleRadians` | radians |

- Angles are stored as **radians internally**. Degrees may appear only in human-facing UI text and importer literals, converted immediately to `AngleRadians` at the boundary.
- Timestamps are never bare integers; the epoch is always declared by the owning `TimeDomain` (sensor clock, GNSS time, scene epoch).

## 3. Quaternions — scalar-last

- Quaternions are stored as `(x, y, z, w)` with the scalar component **last**.
- No other order is valid. Conversion to/from scalar-first or `(w, x, y, z)` storage (e.g. some backends) happens only at adapter boundaries, behind the strict types, and is covered by golden tests.

## 4. Column vectors and transform direction

- Vectors are **column vectors**.
- The transform is written **`p_world = R * p_sensor + t`**, i.e. the transform named **`world_from_sensor`** takes points expressed in the sensor frame into the world frame.
- Reading a transform name as `A_from_B` means: "coordinates in frame B, expressed in frame A". Composition `C_from_A = C_from_B * B_from_A`.

## 5. SE(3)

- A rigid pose is `SE(3) = (R, t)` with rotation `R` (3×3) and translation `t` (3×1), exactly as used in §4.
- Inversion: `world_from_sensor⁻¹ = sensor_from_world = (Rᵀ, −Rᵀ t)`.

## 6. Covariance ordering

- Covariance matrices over pose/transform parameters use **translation-then-rotation** ordering: the 6×6 block is `[t; r]` (translation components first, then rotation/axis-angle components).
- This ordering is fixed for every covariance in the Scene (`pose.covariance`, calibration uncertainty, control-point covariance) and on the wire.

## 7. Camera convention (OpenCV)

- Camera axes: **`+x` right, `+y` down, `+z` forward** along the optical axis.
- **Image origin is top-left**: pixel `(0,0)` is the top-left corner, `u` increases rightward, `v` increases downward.
- Intrinsics use the pinhole-plus-distortion form defined by the declared distortion model (§8).

## 8. Distortion conventions

- Distortion is **declarative**: every camera declares a named distortion model (`opencv`, `opencv_fisheye`, `pinhole`, ...) plus its coefficient vector. Models are stored as data, selected by name from the ratified model registry, never hard-coded per backend.
- Coefficients are interpreted **only** through the declared model. The same coefficient vector under a different model name is invalid.
- Model names are part of the calibration schema (`schemas/**`) and change only through RFC.

## 9. Rendering boundary

- **OpenGL/Vulkan conversion happens at the rendering boundary, never in Core.** Y-flips, handedness changes, and clip-space conventions belong to the viewer/renderer.
- Core, Scene, artifacts, and the SDK always carry the conventions of §1–§8. The rendering boundary is the only place a `WorldFromCamera` becomes a GL/VK matrix.

## 10. Strict domain types

The fixed vocabulary of transform and scalar types (ADR-018):

- `WorldFromCamera`, `CameraFromWorld`, `RigFromSensor`, `SensorFromRig`
- `TimestampNs`, `DistanceMeters`, `AngleRadians`

Rules:

1. Transforms are named by **source and destination frame**, so `RigFromSensor` and `SensorFromRig` are distinct types; the compiler rejects swapped usage.
2. Every transform constructor documents its frame pair; composition and inversion are verified by `check-domain-types` in CI and by property tests (ADR-016).
3. `check-domain-types` (CI gate) rejects raw Eigen matrices leaking into domain code, arithmetic mixing unit types, and untyped frame usage.

## 11. Where raw Eigen / untyped doubles are allowed

- **Raw Eigen matrices and untyped doubles are forbidden in business logic.** They appear only in:
  1. `core/geometry/**` math internals — where the arithmetic actually happens;
  2. inside adapters, behind the adapter boundary (ADR-021);
  3. explicit IO/export boundaries and the Protobuf IPC edge, where the wire format carries units and frames explicitly.
- They never appear in public interfaces, Scene objects, schemas, or the SDK.
- Conversion between strict types and raw values happens **only** at those explicit boundaries.

## 12. Enforcement and testing

- CI gate `check-domain-types` (leak detection, mixed-unit arithmetic, frame-pair documentation).
- Golden tests against reference values for every convention in this document (right-handed, meters, scalar-last quaternions, `world_from_sensor`, OpenCV camera).
- Property-based transform round-trip tests for every strict transform pair.

## References

- ADR-007 (conventions), ADR-018 (strict types), ADR-019 (Eigen behind the strict types)
- `docs/specifications/geometry-model.md`, `docs/specifications/scene-model.md`
- `docs/development/testing.md` (golden + property coverage)
