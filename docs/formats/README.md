# Binary Format Documents — Index

- **Status:** placeholder
- **References:** ADR-010 (artifact store), `docs/specifications/geometry-model.md`
- **Scope:** these documents define the on-disk binary layouts of geometry payload artifacts stored in the CAS.

## Status of this index

The formats below are **defined in the M0 schemas** (`schemas/**`: JSON schema + SQL DDL + protobuf wrappers). The detailed binary layout documents are **planned and will be written post-M0**, when the payloads they describe are first produced. Until then, each entry's normative reference is the schema.

## Planned format documents

Each planned document will specify, per format: byte layout and endianness, header/frame structure, channel semantics and ordering, coordinate-frame and unit declarations, optional/extension blocks, integrity/checksum placement, and the migration path from the M0 schema. None of these documents exist yet; the table below is the intended index.

### How each format anchors in the model

- **Geometry element buffers** back the `GeometryElement` abstraction (`geometry-model.md` §2): every element is a typed handle (`element_id`, `kind`, explicit `frame`, LoD, provenance, payload ref) over an immutable artifact buffer.
- **Point cloud channels** are the payload of `Point` elements: ordered or unstructured positions plus optional per-point channels. Channel identity, order, and units are fixed in schema now; population is deferred (ADR-025).
- **Mesh buffers** are the payload of `Triangle` elements: indexed triangles plus per-vertex attributes. A mesh is a composition of Triangle elements plus topology — never a primitive type on its own.
- **Gaussian splat buffers** are the payload of `Gaussian` elements: per-splat mean, covariance/SH parameters, opacity, color; export must be gsplat-compatible later.
- **Trajectory export** carries dense pose sequences for `Trajectory` entities (kind: odometry/slam/survey/gps), always as strict `WorldFromSensor` poses with an explicit reference frame.

### Common binary conventions (already fixed)

- **Endianness:** little-endian on disk; multi-byte integers and floats are unaligned where required by streaming.
- **Identity:** every buffer artifact carries its SHA-256 in the manifest; readers recompute and verify on read.
- **Units and frames:** channel units (`DistanceMeters`, `AngleRadians`) and the element's `coordinate_frame` are declared in the element metadata, never inferred from the buffer.
- **Optional channels:** a channel may be absent; a buffer never contains unnamed or undeclared channels.

| Document | Payload | Kind(s) | Status |
|---|---|---|---|
| `geometry-element-buffers.md` | generic typed geometry buffers referenced by `GeometryElement` payloads | Point / Triangle / Voxel / Gaussian | defined in M0 schemas; detailed binary layout post-M0 |
| `point-cloud-channels.md` | point positions + optional per-point channels (normals, colors, intensity, confidence, provenance channels per ADR-025) | Point | defined in M0 schemas; detailed binary layout post-M0 |
| `mesh-buffers.md` | indexed triangles + per-vertex attributes, topology/level structure | Triangle | defined in M0 schemas; detailed binary layout post-M0 |
| `gaussian-splat-buffers.md` | per-splat mean, covariance/SH params, opacity, color; gsplat-compatible export later | Gaussian | defined in M0 schemas; detailed binary layout post-M0 |
| `trajectory-export.md` | dense pose sequences for trajectories (kind: odometry/slam/survey/gps) | Trajectory | defined in M0 schemas; detailed binary layout post-M0 |

## Conventions for all format documents

- Payloads are immutable, content-addressed artifacts (SHA-256, ADR-010).
- All buffers conform to the coordinate conventions (`docs/architecture/coordinate-systems.md`): right-handed, meters, scalar-last quaternions, explicit frames.
- Per-point/vertex channel layout is fixed by `schemas/json/geometry-element.schema.json`; channel population is deferred post-M0 (ADR-025).
- Frames must be resolvable within the Scene frame graph at serialization time; unresolved frames fail validation.
- Writers emit payloads only through the CAS atomic temp-then-rename path; readers verify hashes on read and during GC.

## Adding a format

New payload kinds require (a) a ratified element kind or channel addition in the geometry schema, (b) a document in this index, and (c) serialization round-trip + integrity tests. No format is defined by its producer; Core never depends on a vendor's native layout.

## References

- `docs/specifications/geometry-model.md`, `docs/specifications/scene-model.md`
- `docs/architecture/coordinate-systems.md`, `docs/architecture/storage-model.md`
