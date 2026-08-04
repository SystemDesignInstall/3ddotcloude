# Sensor and SensorRig Model Specification

- **Status:** draft (P0)
- **References:** ADR-023, ADR-007, `docs/specifications/scene-model.md`
- **Protected surface:** `core/scene/sensor/**`

This specification defines the **Sensor** and **SensorRig** entities that describe how the physical world was observed. Sensors record the measurements that flow into the Observation Graph (see `scene-model.md` §4.3); a SensorRig describes a set of sensors rigidly coupled in space and time. This model is the metadata contract for capture, calibration, and every downstream algorithm, and is normative for `core/scene/sensor/**`.

## 1. Sensor

| Field | Type | Notes |
|---|---|---|
| `sensor_id` | Uuid | immutable identity |
| `type` | enum | `camera, lidar, imu, gnss, rgbd, panoramic, unknown` |
| `manufacturer` | string | free text |
| `model` | string | free text |
| `serial_number` | string | device serial |
| `time_domain` | TimeDomain | device clock / gps / platform (see §4) |
| `calibration_id` | Uuid? | current calibration, versioned (see §3) |
| `rig_id` | Uuid? | owning SensorRig, if any |
| `source` | ProducerInfo | app + version + git commit that registered the sensor |
| `status` | enum | `active, retired, failed, unknown` |

Rules:

- `sensor_id` is immutable; edits produce a new sensor or a new version, never an in-place mutation.
- All positions, offsets, and times use the platform's strict types — `DistanceMeters`, `TimestampNs`, `AngleRadians` — and never raw numbers on a public interface (ADR-007).

## 2. Sensor types

- **camera**: image sensor with intrinsics (focal length, principal point) and a distortion model (OpenCV pinhole/radial-tangential, fisheye, or Kannala-Brandt). Poses are `WorldFromCamera`; projection follows the OpenCV camera convention (right-handed, x right, y down, z forward).
- **lidar**: active range sensor with a beam layout (scan pattern, vertical field of view, angular resolution), multiple returns per beam (`return_number` / `number_of_returns`), and per-point intensity. Ranges are `DistanceMeters`.
- **imu**: accelerometer + gyroscope with measurement ranges, bias models (constant or time-varying), noise densities, and sample rates (`sample_period_ns`). The IMU provides the temporal backbone for motion estimation and gravity direction.
- **gnss**: position/velocity receiver recording fix types (`none, 2D, 3D, SBAS, DGPS, RTK float, RTK fixed`), satellite counts, and RTK status. Positions are `DistanceMeters` in ECEF; corrections arrive with their own epoch.
- **rgbd**: paired depth and color cameras with a fixed relative pose and a synchronization policy — hardware time-aligned or software alignment of depth and color frames, plus depth units (`DistanceMeters`) and valid range.
- **panoramic**: 360° image sensor with a projection type (`equirectangular` / `cubemap` / other) and horizontal field of view; consumed by downstream stages as derived pinhole views.
- **unknown**: reserved for untyped or third-party sensors; carries only identification fields and is treated as opaque in algorithms.

## 3. SensorRig

A SensorRig couples multiple sensors that are rigidly attached during capture:

| Field | Type | Notes |
|---|---|---|
| `rig_id` | Uuid | immutable |
| `name` | string | display name |
| `sensors[]` | Sensor[] | member sensors |
| `SensorFromRig` | RigFromSensor | rigid transforms per sensor (see below) |
| `time_offsets` | map<SensorId, DurationNs> | per-sensor offset to the rig clock |
| `calibration_id` | Uuid? | versioned rig calibration |
| `source` | ProducerInfo | provenance |

- **Transforms.** Each member has both `RigFromSensor` and `SensorFromRig`, the inverse pair, expressed as quaternion `(x, y, z, w)` + translation in `DistanceMeters`, right-handed meters world convention. The pair is validated to be mutually inverse; the transform data model is strict (ADR-007).
- **Lever arm** and **boresight**: the translation and rotation of a sensor relative to the rig reference frame, used to fuse GNSS/IMU with camera/LiDAR observations.
- **Time offsets.** Each sensor may have a fixed clock offset relative to the rig clock, recorded in `time_offsets` as the data model for TimeDomain alignment (§4); solving offsets is deferred.
- **Versioned calibration.** A rig's calibration is `calibration_id` + calibration version with a validity interval (start/end `TimestampNs`) and full provenance (`source` + contributing calibration artifact hashes). Calibration updates create a new version; past reconstructions always reference the version that was valid at capture time.

### 3.1 Calibration

| Field | Type | Notes |
|---|---|---|
| `calibration_id` | Uuid | immutable |
| `sensor_id` | Uuid | owning sensor |
| `version` | int | monotonically increasing |
| `valid_from` / `valid_to` | TimestampNs | validity interval |
| `intrinsics` | typed | focal length, principal point, distortion coefficients |
| `extrinsics` | WorldFromSensor? | optional rigid pose of the sensor in the rig frame |
| `uncertainty` | json | per-parameter uncertainty block |
| `source` | ProducerInfo | solver/calibrator app + version + git commit + contributing artifact hashes |

Rules:

- Calibration parameters are strict-typed values, never raw matrices (ADR-007).
- A new calibration always gets a new `version` and a new validity interval; it never mutates a previous one.
- A sensor is `calibrated` only when a calibration valid at capture time can be resolved; observations whose capture falls outside every validity interval are flagged as uncalibrated.

## 4. Time domains

- Every sensor records timestamps in exactly one **TimeDomain**: `device` (the sensor's own clock), `gps` (GNSS time), or `platform` (the platform/rig reference clock).
- Internally the platform normalizes all timestamps to `TimestampNs` (nanoseconds) in the platform domain. The mapping from a sensor's native clock to `TimestampNs` is a per-sensor, per-rig offset plus a synchronization record.
- **Domain graph.** Domains and their offsets form a small graph: sensors point at their native domain; the rig points at the platform domain; domains link by offset edges. Any sensor timestamp resolves to `TimestampNs` by traversing this graph.
- **Alignment strategy is data, not math.** The model records the offsets, sync events, and the domain graph; estimation of offsets (synchronization solving) is deferred. Algorithms consuming multi-sensor streams resolve timestamps through this recorded data, and an unresolvable timestamp is a validation error, never a silent guess.

## 5. M0 scope

M0 implements (ADR-031): the full `Sensor` / `SensorRig` / `Calibration` data model with all fields above, strict-type transforms, versioning, time-domain metadata, serialization (SQLite metadata + protobuf), and unit + property tests including the transform-inverse validation. Deferred: solving calibrations (intrinsics, extrinsics, rig calibration), time-offset estimation, and sensor fusion math.

### 5.1 Invariants

1. `sensor_id` and `rig_id` are immutable.
2. Every observation references exactly one sensor with a resolvable calibration valid at capture time.
3. `RigFromSensor` and `SensorFromRig` are exact inverses (property-tested).
4. Every timestamp resolves to `TimestampNs` through the domain graph; unresolvable timestamps are errors.
5. No untyped coordinates or raw matrices on any public interface (ADR-007).
6. Calibrations are append-only and versioned; history is never rewritten.
