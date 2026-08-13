# RFC-0002 — Permanent Spatial Data Model

- **Status:** ratified
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-05
- **Supersedes:** none (extends RFC-0001 baseline)
- **Protected surfaces touched:** Coordinate System (`core/coordinates/**`, `core/geometry/**`), Scene (`core/scene/**`), Observation (`core/scene/observation_graph/**`), GeometryElement (`core/scene/geometry/**`), UUID, Plugin API (`core/plugin/**`, `adapters/interfaces/**`), Capability API (`schemas/**`), Artifact Format (`core/artifacts/**`)

## Summary

Moves the Spatial Platform from an algorithmic pipeline that processes data toward a **permanent spatial data model** with history, provenance, and reproducibility. The Scene gains capture sessions, versioning with a change log, observation relations, geometry identity with provenance, a device/sensor hierarchy, versioned plugin capabilities, and a scalable read index — all ratified here so that post-P2 milestones plug in computational engines without remodeling the data model.

## Motivation

The P2 milestone is the first point at which the Scene becomes a real domain object with sensors, observations, geometry, and query semantics. Every commercial spatial platform (Leica Cyclone, RealityCapture, Bentley iTwin, Trimble Connect, NavVis, FARO Scene, Pix4D, Metashape) treats its Scene as a durable asset that outlives any reconstruction run. Today our model treats a Session as an unmodeled string reference, has no scene version record beyond a bare chain, no change history, and no explicit provenance on geometry identity. Those gaps are structurally cheap to close now and prohibitively expensive after algorithms (SfM, SLAM, ICP, fusion) start writing geometry. ADR-033 deferred version metadata but reserved the model; this RFC fills that reserved surface and adds the ownership and provenance spine the platform needs.

## Problem Statement

1. **No session entity.** `Observation.session_id` and `Frame.session_id` exist as free-form string references (scene.proto), but there is no `CaptureSession` object that groups a device, its sensors, its calibrations, its observations, and its import manifest. Re-scanning a site, merging scans, comparing epochs (ADR-036), or deleting one session without destroying the scene is impossible.
2. **No scene version record.** ADR-033 defines `version_id` / `parent_version_id` / `stage`, but there is no `SceneVersion` carrying a content hash, author, timestamp, and a change list. "Which scene state produced this mesh?" is unanswerable.
3. **No change history.** Nothing records what was added/removed/updated between versions, so audit and rollback reasoning have no substrate.
4. **No device model.** Commercial scanners are devices (Leica BLK2GO, NavVis VLX) composed of many sensors. `Sensor` and `SensorRig` exist; `Device` does not, so real hardware cannot be represented as one unit.
5. **No observation relations.** The Observation Graph (ADR-024) is a set of facts with edges (records, produces, observes, constrains), but no first-class relation records. Derived facts (feature observations, matches, pose constraints) cannot reference their sources.
6. **Geometry identity lacks provenance.** GeometryElement has a provenance block, but identity (persistent/logical/version) is not tied to a structured provenance record, and the producing algorithm is not version-stable.
7. **Capability taxonomy is not versioned.** Extending capabilities would break existing plugins.
8. **No per-scene statistics and no import manifests.** Reproducibility and audit lack cheap counters and per-import provenance records.

## Goals

- Introduce a **permanent, versioned, provenance-rich** Scene model: CaptureSession ownership, SceneVersion + ChangeLog, observation relations, geometry identity + provenance, device hierarchy, sensor health.
- Make `session_id` a **mandatory** attribute of every Observation and Frame.
- Version the capability taxonomy so plugins negotiate against a pinned vocabulary.
- Define a scalable, swappable SceneIndex interface with a spatial slot reserved but not implemented.
- Define reproducibility and stress acceptance criteria.
- Keep M0/P1 exclusion boundaries intact: no algorithms, no KDTree/Octree/BVH/TSDF/mesh/semantic/optimization logic in this RFC's implementation scope.

## Non-Goals

- No algorithmic reconstruction (SfM, SLAM, ICP, BA, fusion) and no algorithm backends.
- No spatial index implementation (R-tree, Octree, NanoFLANN, HNSW) — only the `findSpatial` interface slot.
- No multi-scene branching/merge semantics (deferred by ADR-033); only the version-chain record.
- No Digital Twin epoch processing (ADR-036 defers processing; this RFC only reserves the model surface).
- No copy-on-write scene storage.
- No network/RPC API design.
- No plugin dynamic loading (ADR-034 defers post-M0); static registration only.

## Proposal

### 6.1 SceneVersion

A `SceneVersion` is the immutable record of one state of a Scene (ADR-033 chain + content integrity):

```cpp
struct SceneVersion {
  SceneId   scene_id;
  Uuid      version_id;          // immutable, ADR-033
  Uuid      parent_version_id;   // nil for v1
  uint64_t  revision;            // monotonically increasing
  TimestampNs created_at;
  Hash256   content_hash;        // SHA-256 over the serialized version state
  std::string author;
  std::vector<ChangeRecord> changes;
};
```

- `content_hash` makes "which version produced this mesh" verifiable.
- The version chain is acyclic and append-only (ADR-033 invariant 7).

### 6.2 ChangeLog

Append-only, per-Scene change history. Every mutating operation appends a record:

```cpp
enum class ChangeType {
  ObservationAdded,
  ObservationRemoved,
  GeometryUpdated,
  GeometryAdded,
  CalibrationChanged,
  SensorAdded,
  SensorRemoved,
  SessionAdded,
  ImportExecuted,
  VersionCreated,
};

struct ChangeRecord {
  Uuid        id;
  SceneId     scene;
  TimestampNs timestamp;
  Uuid        object;        // object affected (observation, element, sensor, ...)
  ChangeType  type;
  Hash256     before_hash;   // nil for creation
  Hash256     after_hash;    // nil for deletion
  nlohmann::json payload;    // typed extension data
};
```

- `before_hash`/`after_hash` make the log auditable: a record without hashes cannot prove what changed.
- The ChangeLog is a data table (`scene_change_log`), not an event bus.

### 6.3 CaptureSession (central ownership object)

A CaptureSession groups everything captured in one pass by one device:

```
Scene
└── CaptureSession
      ├── Device ── SensorRig ── Sensor
      ├── Frames
      ├── Observations
      ├── Calibration
      └── ImportManifest
```

```cpp
struct CaptureSession {
  SessionId  session_id;     // mandatory on every Observation and Frame
  SceneId    scene_id;
  DeviceId   device_id;
  TimestampNs started_at;
  TimestampNs ended_at;      // may be nil while open
  Operator      operator;    // compact metadata: id, name
  CaptureSettings settings;  // compact metadata
  EnvironmentalConditions conditions;  // compact metadata
  ImportManifest manifest;   // provenance of this session's import
};
```

Rules:

- Every Observation and Frame **must** reference a `CaptureSession` (validated at write; v1 data migrates into a synthetic legacy session, see §11).
- Sessions enable: re-scanning, merging, epoch comparison, and deleting one session without touching the rest of the scene.

### 6.4 Device / SensorRig / Sensor hierarchy

```
Device (BLK2GO)
└── SensorRig (s)
      └── Sensor (camera / lidar / imu / gnss / ...)
```

```cpp
struct Device {
  DeviceId  device_id;
  std::string name;         // "Leica BLK2GO"
  DeviceKind kind;          // handheld_scanner | mobile_mapping | static |
                            // phone | custom
  std::vector<RigId> rig_ids;
};
```

- A `SensorRig` is a set of rigidly-coupled sensors (sensor-model.md §3); a Device owns one or more rigs.
- Sensor gains `SensorHealth` (operational state, distinct from lifecycle `Sensor.status`):

```cpp
enum class SensorHealth { Ok, Warning, Error, Offline, Calibrating };
struct SensorHealthStatus {
  SensorHealth state;
  TimestampNs updated_at;
  nlohmann::json metrics;   // temperature, drop-out, GNSS fix, IMU saturation, ...
};
```

### 6.5 Observation Graph

Base `Observation` (immutable, ADR-024) extended with **relations**:

```cpp
enum class RelationType { Produces, Derives, Constrains, Calibrates, Synchronizes };

struct ObservationRelation {
  Uuid         relation_id;
  ObservationId source;
  ObservationId target;
  RelationType type;
  nlohmann::json properties;
};
```

- Direction is explicit: `ImageObservation --Produces--> FeatureObservation`, `IMU --Constrains--> Pose`, `Calibration --Calibrates--> Sensor`.
- Relations give the platform a substrate for a processing DAG while keeping observations immutable facts.

### 6.6 Geometry Identity + Provenance

```cpp
struct GeometryIdentity {
  Uuid     persistent_id;   // immutable, never reused
  std::string logical_id;   // stable business key ("south-wall-mesh")
  Version  version_id;      // content version
  GeometryProvenance provenance;
};

struct GeometryProvenance {
  GeometrySource source;    // photogrammetry | lidar | fusion | procedural |
                            // cad | imported | generated | ai
  std::string    algorithm; // free-form string: "COLMAP 5.0", "Gaussian-Splatting-X"
  std::string    plugin_id;
  Hash256        parameters_hash;
  TimestampNs    generated_at;
  std::string    creator;
};
```

- `algorithm` is a **string**, not an enum: future algorithms must not require schema migration.
- A changed mesh yields a new version_id while persistent_id and logical_id survive.

### 6.7 Provenance

- **ImportManifest → Provenance.** Every import records its origin: source format, tool name/version, parameters, and the resulting `GeometryElement`/`Observation` ids.

```cpp
struct ImportManifest {
  Uuid          manifest_id;
  SessionId     session_id;
  std::string   source;         // "Leica BLK2GO export"
  std::string   tool;           // "Leica Cyclone REGISTER 360"
  std::string   tool_version;   // "2026.1"
  std::string   parameters;     // "xyz"
  std::vector<Uuid> result_ids; // produced GeometryElements / Observations
  std::vector<std::string> file_hashes;  // SHA-256 of imported files
  std::vector<std::string> warnings;
  TimestampNs   import_date;
};
```

- **Per-entity provenance** is stored in `scene_provenance` (object_id, source, algorithm, plugin, parameters_hash) so provenance never depends on a side channel.

### 6.8 Plugin Descriptor + Capability Versioning

Extends `AdapterDescriptor` (plugin-api.md §3) into a full `PluginDescriptor`:

```json
{
  "name": "Leica Adapter",
  "version": "1.2.0",
  "license": "proprietary",
  "author": "Leica Geosystems",
  "capability_version": "1.0",
  "capabilities": ["IMPORT_LIDAR", "IMPORT_IMAGE", "IMPORT_IMU", "EXPORT_E57"],
  "dependencies": ["libE57Format 3.1.1"],
  "hardware": ["BLK2GO", "BLK360"],
  "license_reference": "THIRD_PARTY.yml:leica"
}
```

- `capability_version` pins the taxonomy the descriptor was written against; taxonomy changes bump the version and never break old plugins.
- Capability names remain a Constitution-protected, RFC-versioned vocabulary.

## Data Model

Fully specified entity set (all IDs are UUIDs; timestamps `TimestampNs`; transforms strict; ADR-007/018):

- **Scene** — `scene_id`, `schema_version`, `project_id`, `name`, `version_id`, `parent_version_id`, `stage`, `origin`/`crs`, `status`, `statistics`.
- **SceneVersion** — §6.1.
- **ChangeRecord** — §6.2 (stored in `scene_change_log`).
- **CaptureSession** — §6.3.
- **Device / SensorRig / Sensor / SensorHealthStatus** — §6.4.
- **Calibration** — sensor-model.md §3.1 (versioned, validity interval, append-only).
- **CoordinateFrame / FrameGraph / CoordinateAuthority / CoordinateEpoch** — ADR-007; epoch carries a `confidence`/`accuracy` (e.g. RTK 0.015 m, PPP 0.05 m).
- **Observation + 8 subtypes** (Image, LiDAR, IMU, GNSS, Depth, Panoramic, WheelOdometry) **+ ObservationRelation** — §6.5. `WheelOdometryObservation` added to the type set.
- **ObservationQuality** — data structure (sharpness, noise, exposure, motion_blur, confidence); no estimation logic.
- **GeometryElement + GeometryIdentity + GeometryProvenance + GeometrySource** — §6.6; kinds Point/Triangle/Voxel/Gaussian placeholders (geometry-model.md §3).
- **SceneIndex** — §6.8 interface (below).
- **SceneStatistics** — counters (images, lidar frames, imu samples, gnss, geometry count, observation count, coverage), cached and invalidated on mutation.
- **Time model** — `SensorClock`, `ClockDomain`, `TimeOffset`, `TimeSyncRecord`, `LatencyModel` (exposure, rolling-shutter readout, trigger-to-measurement) as data, per sensor-model.md §4.
- **ImportManifest** — §6.7.
- **PluginDescriptor** — §6.8.

### SceneIndex interface

```cpp
class SceneIndex {
 public:
  virtual ~SceneIndex() = default;
  virtual std::vector<Uuid> findById(SceneId scene) const = 0;
  virtual std::vector<Uuid> findBySensor(SensorId sensor) const = 0;
  virtual std::vector<Uuid> findByTimestamp(TimestampNs t) const = 0;
  virtual std::vector<Uuid> findSpatial(BoundingBox box) const = 0;  // reserved
};
```

- `findSpatial` is a reserved slot; the initial implementation reports "not implemented" rather than silently degrading.
- Concrete backends (R-tree, Octree, NanoFLANN, HNSW) plug in later without API change.

## Schema Changes

- **`schemas/protobuf/scene.proto`**
  - `Observation.type` adds `wheel_odometry`.
  - `Observation.session_id` and `Frame.session_id` become **mandatory** (semantically; proto3 strings stay as-is but validation enforces non-empty).
  - New messages: `CaptureSession`, `Device`, `SceneVersion`, `ChangeRecord`, `ObservationRelation`, `ObservationQuality`, `GeometryIdentity`, `GeometryProvenance`, `GeometrySource`, `SensorHealthStatus`, `CoordinateEpoch`, `ImportManifest`, `TimeSyncRecord`, `LatencyModel`, `CoordinateAuthority`, `SceneStatistics`.
- **`schemas/protobuf/errors.proto`** — new error domains: `kTime` (clock resolution, sync), `kSession` (session ownership), `kVersion` (version/change-log), `kChangeLog`.
- **`schemas/json/scene.schema.json`** — mirror the new entities and mandatory `session_id`.
- **`schemas/json/calibration.schema.json`** — unchanged structure; gains references to session and device.
- **`schemas/json/geometry-element.schema.json`** — add `identity` (persistent/logical/version) and `provenance.algorithm` as free-form string.
- **`schemas/json/worker-capabilities.schema.json`** — add `capability_version`.
- **`schemas/json/*`** — plugin descriptor schema for PluginDescriptor (name, version, license, author, capability_version, capabilities, dependencies, hardware).

## Database Migration

`schemas/database/migrations/0002_*.sql` adds **17 tables** (metadata and indices only; payloads stay in the artifact store, ADR-009):

1. `scene_version`
2. `scene_change_log` (id, scene_id, timestamp, operation, object_id, before_hash, after_hash, payload)
3. `scene_provenance` (object_id, source, algorithm, plugin, parameters_hash)
4. `scene_session`
5. `scene_device`
6. `scene_sensor`
7. `scene_rig`
8. `scene_calibration`
9. `scene_observation`
10. `scene_observation_relation`
11. `scene_geometry`
12. `scene_frame`
13. `scene_pose`
14. `frame_graph_edge`
15. `time_sync`
16. `scene_statistics`
17. `import_manifest`

- Migration strategy in code: the runner owns the transaction (ADR-009); version recorded in `schema_meta`.
- No large binary data in SQLite (Constitution §4).

## API Impact

- **C++ kernel** (`spatial-platform/core/scene/**`): new headers and types for every entity in §7; `SceneIndex` interface; read-only Query API (ADR-035) unchanged in spirit, extended with session/version/statistics accessors.
- **Plugin API** (`core/plugin/**`): `PluginDescriptor` replaces/augments `AdapterDescriptor`; capability negotiation reads `capability_version`.
- **Import/export**: every import now creates an `ImportManifest` and a `CaptureSession`.
- **Serialization**: SQLite, JSON, and protobuf round-trips must preserve all UUIDs, hashes, and provenance.
- **Python SDK**: mirrors the read model over the same serialized contract (ADR-035).

## Migration Strategy

- **Breaking changes:** `Observation.session_id` and `Frame.session_id` become mandatory.
- **v1 scenes:** on first open after upgrade, an observation/frame without `session_id` is migrated into a **synthetic legacy session** (`session_id = derived from scene_id`, name "Legacy (pre-RFC-0002)"), preserving data and reproducibility. The synthetic session is flagged in `import_manifest` with `source = "legacy-migration"`.
- The migration is a normal 0002 database migration transaction; it is deterministic and idempotent (rerunnable).
- No data is dropped; existing hashes and content remain valid.

## Compatibility

- **Preserved:** RFC-0001 baseline contracts, UUID/BLOB+string conventions, strict type conventions (ADR-007/018), artifact store (ADR-010), worker IPC (ADR-011/012), scene query read-only surface.
- **Extended:** Scene (session/version/log), Observation (relations + wheel odometry), Geometry (identity/provenance), Plugin (capability versioning).
- **Supersedes:** none. RFC-0002 extends RFC-0001; it does not replace it.
- Old serialized scenes remain readable through the synthetic-legacy-session migration; new fields are additive.

## Alternatives Considered

- **Session as a pure string reference (status quo):** rejected — cannot group device/calibration/observations or support per-session lifecycle.
- **ChangeLog as an event bus / outbox:** rejected — the platform has no broker; a durable data table serves audit and replay without infra.
- **Geometry provenance as only free-form JSON:** rejected — structured `scene_provenance` enables query and enforcement; free-form remains inside structured fields where needed.
- **Capability taxonomy without versioning:** rejected — would break plugins on every taxonomy change (ADR-034).
- **Spatial index now:** rejected — non-goal; reserved interface only (ADR-031 boundary).
- **Device as just another sensor:** rejected — conflates a physical product with a measurement channel; the Device→Rig→Sensor hierarchy matches real hardware.

## Security / Integrity Considerations

- Every version and change record carries content hashes; tampering is detectable at comparison time.
- `parameters_hash` and `file_hashes` give audit evidence for imported data.
- Plugin `capability_version` prevents capability spoofing/desynchronization; license gates remain enforced at selection time (ADR-034).
- No absolute filesystem paths in persisted state (Constitution §4, ADR-008).
- The change log is append-only; there is no in-place mutation path.

## Performance Considerations

- SceneIndex is designed for scalable lookup; initial implementation uses hash-based maps (UUID → entity) with `O(1)` expected lookup and ordered timestamp structures.
- 100M UUID lookups, 10M observations, 100k sensors are explicit stress targets (see Benchmark Plan).
- SceneStatistics are maintained incrementally (cached counters invalidated on mutation), not recomputed on demand.
- SQLite stores metadata/indices only; payloads stay in CAS artifacts (ADR-009/010), so version snapshots are cheap and deduplicated (ADR-033).
- `findSpatial` is a reserved interface; no performance claim is made until a backend exists.

## Benchmark Plan

Separate benchmark target (release build, ADR-029 harness, report per `benchmark-report.schema.json`), **not** part of the ctest suite:

1. **10M Observations** — insert + lookup in SceneIndex; report peak memory and per-op time.
2. **100k Sensors** — load + index by sensor/type.
3. **100M UUID lookups** — SceneIndex `findById` micro-benchmark.
4. **Large-scene serialization** — SQLite 0002 + JSON + protobuf round-trip on a 10M-observation scene.
5. **Import of empty / corrupt / partially-valid data** — ImportManifest validation; correct `kImport`/`kSchema` errors; no crashes.
6. **Mixed-source sessions** (photo + LiDAR + IMU + GNSS in one session) — integration test.
7. **Concurrent access** — 10 readers + 1 writer over SceneIndex; no corruption.
8. **Property-based invariant tests** — graph acyclicity, transform inversion, UUID/identity round-trip (ADR-016).

## Acceptance Criteria

1. A Scene can be created with no geometry (scene-model §9).
2. Geometry can exist without an image (LiDAR-only scene).
3. A Scene can be built from IMU observations alone.
4. A new Observation subtype can be added without modifying Scene (open registration / factory; Open/Closed).
5. The Scene Query API has no knowledge of COLMAP or any adapter.
6. Scene serializes → reads back → all UUIDs preserved (SQLite, JSON, proto).
7. The Frame Graph is acyclic (cycle is a validation error).
8. Every Observation resolves its coordinate frame, timestamp, sensor, session, and UUID (invariant §8.1 extended with session).
9. **Reproducibility:** import Dataset A → generate geometry → save scene → reload → generate the same geometry → `SceneVersion.content_hash`, `GeometryIdentity`, and `Provenance` are identical.
10. P2.5 stress benchmarks (Benchmark Plan) pass their targets without corruption under concurrency.

## Implementation Plan

Implementation order follows dependency direction (CONSTITUTION §5): coordinates → geometry transforms → sensor/time → session → observation graph → geometry identity → scene → query → plugins.

- **P2a:** `core/coordinates/` (CoordinateAuthority, CoordinateEpoch, CoordinateFrame, FrameGraph) + `core/geometry/` (SE3, transform types, quaternion conventions) + `core/scene/sensor/time/` (SensorClock, ClockDomain, TimeSyncRecord, LatencyModel). Property tests: `transform * inverse(transform) == identity`, `frame_graph is acyclic`, `timestamp conversion round-trip`.
- **P2b:** sensor model, device hierarchy, capture session, calibration, ids.
- **P2c:** versioning + changelog, observation graph, geometry identity, scene index.
- **P2d:** query API, plugin descriptor, acceptance tests.
- **P2.5:** benchmark target + stress + concurrency.

Each stage runs the constitution, domain-type, dependency, schema, and architecture-debt gates; Debug and Release builds must pass 100% of tests.

## References

- `docs/specifications/scene-model.md` (ADR-023/024/032/033/035/036)
- `docs/specifications/sensor-model.md` (Device, Time domains, SensorRig)
- `docs/specifications/geometry-model.md` (GeometryElement, identity)
- `docs/specifications/plugin-api.md` (AdapterDescriptor, capability negotiation)
- `docs/architecture/coordinate-systems.md` (ADR-007/018)
- ADR-033 (immutable scene versioning), ADR-034 (capability plugin architecture), ADR-036 (digital twin temporal epochs), ADR-031 (M0 scope boundary)
- `schemas/database/migrations/0001_init.sql`, `schemas/protobuf/scene.proto`
- CONSTITUTION.md §1 (principles), §2 (protected surfaces), §5 (change control)
