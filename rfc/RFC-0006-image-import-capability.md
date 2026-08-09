# RFC-0006 — Image Import Capability

- **Status:** ratified
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-09
- **Supersedes:** none
- **Depends on:** RFC-0002, PPS-0001, ADR-004, ADR-006, ADR-007, ADR-009, ADR-010, ADR-014, ADR-015, ADR-020, ADR-021, ADR-023, ADR-024, ADR-033, ADR-034, ADR-035
- **Protected surfaces touched:** Capability API (`schemas/**` — adds `import` to `worker-capabilities.schema.json`, new `image.schema.json`), Scene (`core/scene/**` — new CaptureSession/Frame/ImageObservation write types, `core/scene/identity.h`), Observation (`core/scene/observation_graph/**` — `ImageObservation`), Artifact Format (`core/artifacts/**` — new `PutInstance`), UUID (`core/**` type `Uuid` — `GenerateUuidV5`)

## Summary

RFC-0006 ratifies the **Import capability** and the P2.1 image-ingestion foundation: the `import` capability is added to the capability taxonomy, and the platform gains the canonical types, identity scheme, and artifact-store operation needed to ingest image files into a permanent Scene (RFC-0002). It enables one accepted image to produce exactly three canonical records — an immutable **ImageArtifact** (byte-exact payload in CAS, ADR-010), a **Frame**, and an **ImageObservation** — inside a **CaptureSession**, with deterministic v5 identity (PPS-0001 §5.4), full provenance (§5.7), content-addressed deduplication at the payload layer, and per-file failure semantics. No new database migrations are introduced: the required tables already exist in migration `0001` (`capture_sessions`, `frames`, `observations`, `observation_payloads`, `scenes`, `scene_versions`, `sensors`, `calibrations`) and receive their first typed accessors. Import never reconstructs (PPS-0001 §5.9, Principle 16): it records observations permanently and leaves interpretation to replaceable engines.

## Motivation

P2.1 is the first consumer of the Permanent Spatial Data Model (RFC-0002). Before any photogrammetry engine (COLMAP per ADR-004) can run over a Scene, image bytes and their capture context must enter the model through a canonical, provenance-complete, deterministic path. Today the platform has no such path: the CLI reads files into the CAS as generic `type: "image"` artifacts with no Frame, no ImageObservation, no CaptureSession, no sensor/calibration resolution, and no error taxonomy. Three structural gaps block P2.1 implementation:

1. **The `import` capability does not exist in the frozen taxonomy.** `worker-capabilities.schema.json` and `plugin-api.md` §4 enumerate ten capabilities (sparse_reconstruction … gnss_integration); both `reconstruction-pipeline.md` §2.1 and `image-import.md` §2 reference an `Import` capability that is absent. `plugin-api.md` §4 and `adding-adapter.md` require an RFC before the taxonomy is extended (Capability API is Constitution-protected, CONSTITUTION §2).

2. **Identity primitives are incomplete.** `core/utils/uuid.h` implements only UUIDv4 generation. The P2.1 contract (§6) requires deterministic FrameID/ObservationID via UUIDv5 over a canonical name `(<prefix>|<sensor_id>|<timestamp_ns>|<content_hash>)`, stable across re-imports, machines, and processes (PPS-0001 §5.4, §6.3).

3. **The artifact store cannot express "same content, new instance."** `ArtifactStore::Put` deduplicates a content-identical write by returning the existing hash with `deduplicated=true` and writes no new manifest. The P2.1 contract (§13 case 2) requires content-identical but context-different imports to create a **new artifact instance** (new random `artifact_uuid`, its own manifest and provenance) while sharing the single stored CAS payload (ADR-010). This is a genuinely new store operation.

This RFC closes these three gaps so the implementation plan can proceed in dependency order (CONSTITUTION §5) against protected surfaces.

## Design

### 6.1 Capability taxonomy: add `import`

`schemas/json/worker-capabilities.schema.json` adds `"import"` to the capabilities enum (alphabetical position first). Semantics: a worker/adapter declaring `import` can ingest source files into canonical artifacts/observations; the engine selects it by capability (ADR-034, Principle 8), never by vendor name. The taxonomy remains RFC-versioned; the `capability_version` mechanism reserved by RFC-0002 §6.8 governs future negotiation.

```json
"enum": [
  "import",
  "sparse_reconstruction",
  "dense_stereo",
  "bundle_adjustment",
  "icp",
  "surface_reconstruction",
  "texturing",
  "gaussian_generation",
  "lidar_odometry",
  "loop_closure",
  "gnss_integration"
]
```

### 6.2 `image.schema.json`

Formalized at `schemas/json/image.schema.json` from the inline draft in `image-import.md` §16 (draft 2020-12, `additionalProperties` implicit-true for forward-compatible extension; `required` lists `artifact_uuid`, `content_hash`, `type`, `schema_version`, `producer`, `configuration_hash`, `creation_timestamp`, `coordinate_frame`, `unit`, `file_size`, `mime_type`, `validation_status`, `width`, `height`, `pixel_format`). `type` is `const "image"`, `schema_version` is `const 1`, `content_hash`/`configuration_hash` are `^[0-9a-f]{64}$`, `width`/`height` ≥ 1, `validation_status` ∈ {`valid`, `degraded`, `unverified`}, optional `exif` and `band` objects. The format is frozen as an immutable, extensible contract per the schema-evolution policy established in RFC-0005: additive keys only; never change semantics in place.

### 6.3 UUIDv5 identity

`core/utils/uuid.{h,cpp}` gains `Uuid GenerateUuidV5(const Uuid& namespace_uuid, const std::string& name)` implementing RFC 4122 §4.3 (SHA-1, version 5, variant 10). The reserved platform namespace UUID is fixed in `core/scene/identity.h` (identical across runs and installations, per `image-import.md` §6). Identity derivation:

```
name := <prefix> "|" <sensor_id> "|" <timestamp_ns> "|" <content_hash>
FrameID       = UUIDv5(PLATFORM_NAMESPACE, "frame|"       + name)
ObservationID = UUIDv5(PLATFORM_NAMESPACE, "observation|" + name)
```

Serialization stays lowercase hyphenated (ADR-0001 UUID conventions). Consequences per `image-import.md` §6: identical (sensor, timestamp, content) tuples map to identical IDs across re-imports; differing context maps to differing IDs (the dedup case of §13). This is purely additive — existing UUIDv4 generation is unchanged.

### 6.4 Scene write types (`core/scene/**`)

Minimal immutable write types needed for ingestion (RFC-0002 §6.3, §6.5; `scene-model.md` §4.2, §4.4):

- **CaptureSession** — `session_id` (UUIDv4, instance identity), `scene_id`, `name`, `source_uri`, `started_at`, `ended_at`, `provenance`. Resolved if explicit, else created per batch (image-import.md §7).
- **Frame** — `frame_id` (v5), `session_id`, `timestamp_ns`, `sequence_index`, `sensor_id`, `pose_ref` (null at import), `properties_json`.
- **ImageObservation** (in `core/scene/observation_graph/**`) — `observation_id` (v5), `frame_id`, `session_id`, `artifact_ref`, `width`, `height`, `pixel_format`, optional `focal_prior`/`pose_prior`/`exposure_ns`/`band`, `properties_json`. `artifact_ref` is the ImageArtifact's `artifact_uuid`, completing the chain bytes → artifact → observation → frame.

Types are immutable once written (PPS-0001 §5.3, ADR-024); a correction is a new observation. Foreign fields (vendor blocks, raw EXIF) live in `properties`/provenance only (image-import.md §4).

### 6.5 `ArtifactStore::PutInstance` (`core/artifacts/**`)

New store operation enabling dedup case 2 without mutating existing artifacts:

```
ArtifactWriteResult PutInstance(const Hash256& existing_content_hash,
                                const ArtifactManifest& manifest);
```

- Precondition: the payload identified by `existing_content_hash` exists in the store (verified).
- Writes a **new manifest** at `artifacts/<new artifact_uuid>/manifest.json` referencing the existing shared payload at `artifacts/cas/<hash[0:2]>/<hash>` (artifact-format.md §2, ADR-009/010). The payload is never re-written, re-encoded, or touched.
- Returns `ArtifactWriteResult{ existing_content_hash, new artifact_uuid, deduplicated=true }`.
- The pre-existing artifact instance is **never mutated** (PPS-0001 §5.3, Principle 7): the two instances coexist with identical content hash, independent `artifact_uuid`, and independent provenance.
- `Put(payload, manifest)` behavior is unchanged (dedup case 1 / new-content path).

### 6.6 IMPORT error codes (`core/errors/project_error.h`)

Under the existing `ErrorDomain::kImport` (6), machine-readable codes per image-import.md §12: `IMPORT_UNREADABLE`, `IMPORT_CORRUPT`, `IMPORT_UNSUPPORTED_FORMAT`, `IMPORT_MISSING_EXIF`, `IMPORT_SENSOR_UNRESOLVED`, `IMPORT_TIMESTAMP_UNRESOLVABLE`, `IMPORT_VALIDATION_ERROR`. Errors are per-file: a failed file never partially writes (no frame, no observation, no artifact), and the stage persists successfully imported observations and continues (ADR-014).

### 6.7 Metadata accessors (`core/storage/metadata_db.*`)

First typed accessors for the already-schema-resident tables (migration `0001`; **no new migrations**): resolve-or-create `capture_sessions`; insert `frames`, `observations`, `observation_payloads`; resolve a `sensor_id` to a registered sensor with a calibration valid at capture time (`sensor_unresolved` / `calibration_unresolved` → observation flagged `uncalibrated`, a warning not a failure, sensor-model.md §3.1); create/advance `scenes` + `scene_versions` at stage `imported` (ADR-033). `core/storage/` is not a Constitution-protected prefix; the accessors implement RFC-0002 entities.

### 6.8 Import boundary (`importers/images/**`)

The importer is the only place that touches a source file (image-import.md §2). It reads bytes, detects format by magic bytes (JPEG/TIFF/PNG/EXR/RAW), reads header-only geometry (`width`, `height`, `pixel_format`), computes SHA-256, resolves session/sensor/timestamp, calls `Put` or `PutInstance` per the dedup policy (§13), writes Frame + ImageObservation, and records provenance. RAW families are imported as **opaque byte payloads** — never decoded at import (image-import.md §14). No external image library crosses the boundary (PPS-0001 §5.1); header parsing is implemented inside `importers/images/**`. EXIF extraction is deferred (contract §5 optional fields are captured as available; full EXIF is a later increment).

## Compatibility

- **No breaking change.** `import` is added to an enum; existing worker declarations that name no `import` capability are unaffected. Existing CAS artifacts and manifests remain valid; migration `0001` tables are untouched (no new DDL).
- **Additive only.** UUIDv5 and `PutInstance` extend existing APIs; `Put`, UUIDv4, and all existing store semantics are unchanged.
- `image.schema.json` is a new schema; existing generic `type: "image"` artifacts (pre-P2.1 CLI path) remain readable, and new imports validate against the richer schema.
- The "reserved for RFC-0004" comment on capability taxonomy in `plugin-api.md` remains valid: RFC-0004 stays reserved for the plugin/worker ecosystem; this RFC extends the taxonomy once, under the RFC-track rule it itself mandates.

## Alternatives

- **Add `import` via a vendor importer instead of a capability.** Rejected: violates ADR-034 and Principle 8; the engine must select implementations by capability, and reconstruction-pipeline.md §2.1 already models Import as a capability.
- **Reuse `Put` for dedup case 2 by returning the existing artifact.** Rejected: violates the P2.1 contract (§13) and PPS-0001 §5.3 — content-identical but context-different captures must remain distinct immutable instances with independent provenance; reusing the existing instance would conflate two capture contexts.
- **Implement UUIDv5 inside the importer, not in `core/utils`.** Rejected: identity is a core primitive (PPS-0001 §5.4); determinism must be shared, testable, and reusable by every future importer (LiDAR, video, GNSS).
- **Decode images at import (width/height/pixel_format via a decoding library).** Rejected: RAW must stay opaque (image-import.md §14); no transcode at the adapter boundary (PPS-0001 §5.2); header-only parsing keeps the dependency surface (THIRD_PARTY.yml) untouched.
- **New DB migrations for import tables.** Rejected: all required tables exist in migration `0001`; adding migrations would violate the plan's no-new-migrations constraint and duplicate existing DDL.

## Open Questions

- EXIF-derived metadata (make/model/exposure/GPS) remains optional in P2.1; the schema reserves the `exif` object and the importer records EXIF *as available*, with full extraction deferred to a later increment.
- Whether the `import` capability should be advertised by a future dedicated worker or folded into an existing runner is a deployment concern outside this RFC; the taxonomy entry is what this RFC fixes.

## Impact

- **Modules:** `core/utils/uuid.{h,cpp}` (`GenerateUuidV5`), `core/scene/identity.h` (namespace UUID + name derivation), new `core/scene/capture_session.h`, `core/scene/frame.h`, `core/scene/observation_graph/image_observation.h`, `core/storage/metadata_db.{h,cpp}` (session/frame/observation/sensor/scene accessors), `core/artifacts/artifact_store.{h,cpp}` (`PutInstance`), `core/errors/project_error.h` (`IMPORT_*`), `importers/images/**` (image_format + image_importer), `cli/main.cpp` (`spatial import`).
- **Schemas:** `schemas/json/worker-capabilities.schema.json` (`import`), new `schemas/json/image.schema.json`.
- **Docs:** `docs/specifications/image-import.md` is the governing P2.1 contract; RFC-0006 ratifies the surfaces it requires.
- **Tests:** `tests/unit/test_uuid_v5.cpp`, `tests/unit/test_image_format.cpp`, `tests/unit/test_image_importer.cpp`; dedup/identity/failure-path coverage per image-import.md §16.
- **Acceptance:** a supported file imports to one ImageArtifact + Frame + ImageObservation inside a session with stable v5 IDs and full provenance; two content-identical files in different contexts yield one CAS payload, two instances, two observations; re-import of an identical tuple is idempotent; corrupt/unsupported files fail per-file without aborting the stage; no new DB migrations; `check_constitution --rfc RFC-0006` passes.

## References

- `docs/specifications/image-import.md` (P2.1 contract, PPS-0001 first consumer)
- `docs/PPS-0001-platform-principles.md` (§5.1–§5.4, §5.7, §5.9, §6.3)
- RFC-0002 (§6.3 CaptureSession, §6.5 Observation, §6.8 capability versioning), RFC-0005 (schema-evolution policy), RFC-0003
- ADR-004, ADR-006, ADR-007, ADR-009, ADR-010, ADR-014, ADR-015, ADR-020, ADR-021, ADR-023, ADR-024, ADR-033, ADR-034, ADR-035
- CONSTITUTION.md §1 (principles 7, 8, 10, 12, 16), §2 (protected surfaces), §5 (change control)
- `schemas/database/migrations/0001_init.sql`, `schemas/database/schema.sql`
