# Scene Model Specification

- **Status:** ratified (P0)
- **References:** ADR-023, ADR-024, ADR-032, ADR-033, ADR-035, ADR-036, ADR-006, ADR-007, ADR-018, ADR-009, ADR-010
- **Protected surface:** `core/scene/**`, `core/scene/observation_graph/**` (Constitution §2)

## 1. Purpose

Defines the **Scene**, the central domain object of the Spatial Platform. Every algorithm, pipeline, product, and provenance chain operates on a Scene. The Scene is **algorithm-independent**: it exists before, during, and after any reconstruction, and every geometry provider produces data that conforms to it.

This specification is normative for `core/scene/**`. It is the cross-language contract for `spatial-platform` (C++/Python) — the `.spx` storage and the serialized forms must satisfy it.

## 2. Relationship to Project

- **Project** is the storage container (a `.spx` directory, ADR-008). It owns the filesystem layout, the SQLite metadata database, the artifact store, and the list of scenes.
- **Scene** is the domain root. It owns all domain state: sensors, frames, poses, observations, geometry, appearance, semantics, and quality.
- A Project may contain one or more Scenes. M0 supports a single primary scene per project; multi-scene workflows (branching, merging, separate survey regions) are allowed by the data model (ADR-033).
- Identity: `SceneId` (UUID) is independent of `ProjectId` (UUID). Both are immutable.

## 3. Scene Structure

```
Scene
├── Kinematics         Frames, Poses, Trajectories
├── Sensors            Sensor Graph (sensors, rigs, calibration)
├── Measurements       Observation Graph (observations, constraints)
├── Geometry           Geometry Graph (GeometryElement tree)
├── Appearance         textures, materials, Gaussian appearance
├── Semantics          semantic objects, labels, instances
├── Control            control points, survey constraints
└── Quality            uncertainty, quality reports, metrics
```

The three graphs are the backbone (ADR-024, ADR-032):

```
Scene
├── Observation Graph      what was measured, by whom, when
├── Geometry Graph         geometry elements and their relationships
└── Relationship Graph     links between observations, geometry, objects
```

They are stored as index/edge structures in the metadata database; payloads (images, point buffers, mesh data) live in the artifact store (ADR-009, ADR-010).

## 4. Entities

### 4.1 Scene

| Field | Type | Notes |
|---|---|---|
| `scene_id` | Uuid | immutable |
| `schema_version` | int | scene schema version, migrates independently |
| `project_id` | Uuid | owning project |
| `name` | string | mutable display name |
| `version_id` | Uuid | current scene version (ADR-033) |
| `parent_version_id` | Uuid? | lineage |
| `stage` | enum | lineage stage label: `created, imported, aligned, dense, meshed, textured, gaussian, finalized` |
| `created_by` | ProducerInfo | app + version + git commit |
| `created_at` | TimestampNs | wall clock |
| `origin` | SceneOrigin | reference frame + CRS for the scene |
| `status` | enum | `open, read_only, archived` |
| `properties` | json | free-form project-defined metadata |

### 4.2 Frames, Poses, Trajectories (Kinematics)

- **Frame**: `frame_id`, `session_id`, `timestamp_ns` (monotonic in the sensor's TimeDomain), `sequence_index`, `sensor_id`, `pose_ref`.
- **Pose**: `pose_id`, `frame_id`, transform `WorldFromSensor`, `covariance` (6x6, translation-then-rotation ordering), `source` (producer + provenance), `status` (raw estimate / optimized / anchored), `quality` reference.
- **Trajectory**: `trajectory_id`, `session_id`, ordered `frame_id[]`, `kind` (odometry/slam/survey/gps), `artifact_ref` for the dense pose sequence, `confidence`.

### 4.3 Sensors and Rigs (Sensor Graph)

Per ADR-023 and the Sensor Model Specification (`docs/specifications/sensor-model.md`):

- **Sensor**: `sensor_id`, `type` (camera, lidar, imu, gnss, rgbd, panoramic, unknown), `manufacturer`, `model`, `serial_number`, `time_domain`, `calibration_id`, `rig_id`.
- **SensorRig**: `rig_id`, `name`, `sensors[]`, rigid transforms `RigFromSensor`, `SensorFromRig`, time offsets, lever arms, boresight, versioned calibration.
- **Calibration**: `calibration_id`, `sensor_id`, `version`, model-specific parameters (intrinsics, distortion model + coefficients, extrinsics), `uncertainty`, `calibration_time`, `source`.

The Sensor Graph is a view of the Scene's frame graph restricted to rigidly-coupled sensor frames.

### 4.4 Observations (Observation Graph)

The Observation Graph is the **canonical measurement substrate** (ADR-024). It exists before any algorithm runs.

**Observation** (base, immutable — Architecture Principle 3):

| Field | Type | Notes |
|---|---|---|
| `observation_id` | Uuid | immutable |
| `scene_id` | Uuid | |
| `sensor_id` | Uuid | which sensor recorded it |
| `frame_id` | Uuid? | kinematic frame (may be null for stateless measurements) |
| `session_id` | Uuid? | capture session |
| `timestamp_ns` | TimestampNs | capture time |
| `artifact_ref` | ArtifactRef? | payload (image file, scan file, etc.) |
| `source` | SourceRef | importer/producer + version + git commit |
| `provenance` | ProvenanceRef | input artifacts + config hash |
| `properties` | json | typed extension data |

Subtypes (all immutable):

- **ImageObservation**: `width`, `height`, `pixel_format`, `focal_prior` (DistanceMeters?/pixels), `pose_prior`, `distortion_model`, `exposure_ns`, `band` (rgb/nir/...).
- **LiDARObservation**: `return_number`, `number_of_returns`, `range_unit` (DistanceMeters), `intensity`, `scanner_local_frame`.
- **IMUObservation**: `accelerometer_xyz` (m/s^2), `gyroscope_xyz` (rad/s), `temperature`?, `sample_period_ns`.
- **GNSSObservation**: `position_ecef` (DistanceMeters), `position_covariance`, `velocity`, `fix_type`, `satellites`, `rtk_status`, `epoch`.
- **DepthObservation**: `format` (float16/float32/uint16), `units` (DistanceMeters), `valid_range_min/max`, `confidence_artifact_ref?`.
- **PanoramicObservation**: `projection` (equirectangular/cubemap), `width`, `height`, `horizontal_fov`.

**Graph semantics:**

- Edges: `(Frame) — records — (Observation)`, `(Sensor) — produced — (Observation)`, `(Observation) — observes — (GeometryElement)`, `(Observation) — constrains — (GeometryElement)`.
- An observation is a **fact**, never mutated. Corrections produce new observations (and new scene versions).
- GTSAM (ADR-005) and all optimization backends operate **on** this graph: the graph is data, factors are algorithms.

### 4.5 Geometry (Geometry Graph)

Per the Geometry Model Specification (`docs/specifications/geometry-model.md`) and ADR-032.

- The Scene holds a **Geometry Graph**: a hierarchy of `GeometryElement`s with explicit relationships (containment, registration, LoD, correspondence).
- M0 stores the `GeometryElement` base plus placeholder concrete types (`Point`, `Triangle`, `Voxel`, `Gaussian`) — the structure is normative; the production logic is deferred.
- Every geometry element MUST carry an explicit `coordinate_frame` (Architecture Principle 13). An element without a frame is a defect.

### 4.6 Appearance and Semantics

- **Appearance** (deferred implementation, structure normative): texture sets, materials, PBR channels, Gaussian appearance parameters. Referenced per geometry element or per LoD.
- **SemanticObjects** (deferred): `object_id`, `class`, `instance`, `geometry_ref`, `mask_refs[]`, `attributes{}`. The Scene reserves the slot; capture of semantics is a later milestone (ADR-036 notes the temporal model).

### 4.7 Control Points and Survey

- **ControlPoint**: `control_point_id`, `name`, `coordinates` in a `CoordinateReferenceSystem`, `coordinates_covariance`, `constraint_type` (xyz/xy/z), `observation_refs[]` (image/scan measurements), `status` (unused/used/adjusted), `source`.

### 4.8 Quality

- **QualityReport** (per scene or per object): metrics block (coverage, baseline, view angle, expected error, expected mesh/gaussian quality per ADR-030), `uncertainty` summary, `artifact_ref` to the machine-readable report.
- Quality metadata is **first-class data** (Architecture Principle 14), carried with the scene, not derived on demand.

## 5. Scene Versioning (ADR-033)

- Every mutating operation creates a new **scene version**: `v1 → import → v2 → align → v3 → dense → v4`.
- `Scene.version_id` + `parent_version_id` form the lineage chain.
- M0 stores only the version-chain metadata (see 4.1). Full copy-on-write semantics, branching, and comparison are deferred, but the model must not need migration to add them (ADR-033).
- Scene versions reference immutable artifacts, so snapshots are cheap (CAS deduplication, ADR-010).

## 6. Scene Query API (ADR-035)

A unified, **read-only** query surface over the Scene. Internal storage is replaceable without touching consumers.

```cpp
scene.query().images();                       // ImageObservation view
scene.query().lidar();                        // LiDARObservation view
scene.query().imu();                          // IMUObservation view
scene.query().gnss();                         // GNSSObservation view
scene.query().depth();                        // DepthObservation view
scene.query().frames();                       // frames + poses
scene.query().points();                       // Point geometry elements
scene.query().geometry();                     // all GeometryElements
scene.query().visibleFrom(camera);            // elements visible from a camera
scene.query().observedBy(observation);        // elements observed by a measurement
scene.query().sensors();                      // sensor graph
scene.query().quality();                      // quality metrics
scene.query().uncertainty();                  // uncertainty/confidence summary
scene.query().version();                      // version metadata
```

- The API returns **views/handles**, never mutable core storage.
- All queries are deterministic and side-effect free.
- Python SDK mirrors a subset (`scene.query.images()` etc.) over the same serialized contract.

## 7. Serialization and Storage

- Scene metadata → SQLite (`.spx/project.db`), tables prefixed `scene_`, `obs_`, `pose_`, `geom_` (schema in `schemas/database/scene.sql`).
- Scene payloads → artifact store (CAS, ADR-010): images, point buffers, meshes, textures, gaussians.
- Serialized scene snapshots → protobuf `Scene` message (for IPC and portability) and JSON (for diagnostics), both under `schemas/protobuf/scene.proto` and `schemas/json/scene.schema.json`.
- Serialization round-trips are property-tested (ADR-016).

## 8. Invariants

1. Every observation has exactly one `sensor_id` and one `timestamp_ns`.
2. Observations are immutable once written (corrections create new observations).
3. Every geometry element has an explicit `coordinate_frame`.
4. No large binary data in SQLite (ADR-009).
5. No raw Eigen matrices / untyped coordinates in any Scene public interface (ADR-007, ADR-018).
6. Every `Pose`, `GeometryElement`, and `QualityReport` records `source` provenance.
7. The scene version chain is acyclic and grows by append only.
8. AI outputs appear in a Scene only as priors attached to observations, never as authoritative geometry (ADR-006).

## 9. M0 Scope

M0 implements (ADR-031): Scene entity + version metadata, Frames/Poses/Trajectories, Sensors/Rigs/Calibration, the full Observation hierarchy and Observation Graph, GeometryElement base + placeholder types, Scene Query API, and serialization, with unit + property tests. Deferred: Appearance/Semantics content, Geometry Graph logic, Relationship Graph logic, copy-on-write versions, Digital Twin epochs.
