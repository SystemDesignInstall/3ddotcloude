# Image Import Specification (P2.1)

- **Status:** draft (P2)
- **References:** PPS-0001, RFC-0002, ADR-004, ADR-006, ADR-007, ADR-009, ADR-010, ADR-014, ADR-015, ADR-020, ADR-021, ADR-023, ADR-024, ADR-033, ADR-034, ADR-035, `docs/specifications/reconstruction-pipeline.md`, `docs/specifications/scene-model.md`, `docs/specifications/sensor-model.md`, `docs/specifications/artifact-format.md`
- **Protected surface:** `importers/images/**`, `core/scene/**`, `core/artifacts/**`, `schemas/**` (Constitution §2)

This document is the P2.1 contract for the **Import** stage (`reconstruction-pipeline.md` §2.1) as applied to still images. It fixes the canonical output model, identity scheme, provenance, and failure semantics for ingesting image files. It is the first concrete consumer of PPS-0001 and must be read together with it.

## 1. Purpose and Scope

**Purpose.** Ingest raw image files into a Scene without altering them. The importer reads image bytes and capture metadata, writes an exact byte copy into the artifact store (CAS), and records one canonical `Frame` + one `ImageObservation` per accepted image inside a `CaptureSession`.

**In scope.** Still image files: JPEG, TIFF, PNG, OpenEXR, and RAW families (CR2, NEF, ARW, DNG). RAW files are imported as **opaque byte payloads** — never decoded at import time (see §14).

**Out of scope (P2.1):**

- Video frame extraction (MP4/MOV) — a future importer family (`importers/video/**`).
- RAW decoding and demosaicing — a future decode capability; not required to make a RAW byte copy addressable and provenance-complete.
- Any photogrammetry algorithm (feature extraction, matching, pose estimation). The importer only records; it never reconstructs (PPS-0001 §5.9 Algorithm Independence).
- Any vendor format promotion into the Canonical Spatial Model (PPS-0001 §5.1). Foreign types stop at the adapter boundary.

## 2. Pipeline position

Image import is a **Capability**, not a vendor importer. The boundary is strict:

```
Scheduler → Capability → Adapter → Canonical Artifact
```

- The platform selects an importer because it implements the `Import` capability (ADR-034), never because of a vendor name (`adding-adapter.md`).
- The importer is the **only** place that touches a source file. The Canonical Spatial Model consumes exactly what this spec defines and nothing else.
- This is **not**:

```
Image Files → COLMAP → COLMAP database
```

COLMAP is the default SfM *adapter* (`reconstruction-pipeline.md` §2.3, ADR-004) and runs later, over canonical observations — it has no role in ingestion. Import and reconstruction are separated by an immutable scene version (ADR-033).

## 3. Input contract

| Input | Meaning |
|---|---|
| Image file | raw bytes on disk or streamed from a source URI; read-only, never mutated |
| Source URI | original location (path, file://, or capture-package key) |
| Capture context | `sensor_id` (which camera recorded it), capture time, and session (if an explicit one is provided) |
| Sensor / calibration records | registered sensors and their calibrations with validity intervals (`sensor-model.md` §3) |
| Importer configuration | format allowlist, EXIF policy, RAW policy, explicit session override |

- Files may arrive individually or as a batch; the batch is the unit of session grouping (§7).
- Accepted MIME families: `image/jpeg`, `image/tiff` (incl. DNG), `image/png`, `image/x-exr`, RAW families by magic bytes (CR2/NEF/ARW/DNG). Format detection is by magic bytes, not by file extension.
- An input file that matches no allowlist entry is rejected per §14.

## 4. Canonical output model

One accepted image produces exactly three canonical records, all inside one `CaptureSession`:

```
ImageArtifact ── payload  → original bytes (CAS, immutable)
Frame          ── kinematic frame for the exposure
ImageObservation ── scene record (scene-model.md §4.4)
```

**ImageArtifact** — content-addressed payload + manifest (see §8, §9). Identity: `content_hash = SHA-256(bytes)`; instance id: `artifact_uuid`.

**Frame** (`scene-model.md` §4.2): `frame_id`, `session_id`, `timestamp_ns`, `sequence_index`, `sensor_id`, `pose_ref` (null at import — no pose is estimated here).

**ImageObservation** (`scene-model.md` §4.4): base fields + image subtype fields `width`, `height`, `pixel_format`, and optional `focal_prior`, `pose_prior`, `distortion_model`, `exposure_ns`, `band`. `artifact_ref` points at the ImageArtifact.

- Records are **immutable** once written (PPS-0001 §5.3, ADR-024): a correction is a new observation, never an edit.
- The importer writes **only** canonical records. Foreign fields (vendor blocks, raw EXIF blobs) go into `properties`/`provenance`, never into the canonical schema.

## 5. Required metadata

| Class | Fields |
|---|---|
| **Required** | `sensor_id` (resolvable to a registered camera sensor); `timestamp_ns` (capture time resolved to the platform time domain); resolvable calibration valid at capture time — otherwise the observation is imported flagged `uncalibrated` (`sensor-model.md` §3.1), which is a warning, not a failure; `width`, `height`, `pixel_format` (from header/decode); session membership (§7) |
| **Optional** | EXIF-derived: make, model, lens model, focal length, exposure time, ISO/gain, GPS position, capture-time string; `focal_prior`, `pose_prior`, `distortion_model`, `exposure_ns`, `band` |
| **Derived** | `content_hash`, `file_size`, `mime_type`, `frame_id`/`observation_id` (v5, §6), `creation_timestamp` |

- A missing required field other than calibration is a validation error (§12).
- EXIF is a **prior source**, never authoritative geometry (ADR-006). EXIF GPS and focal length enter as priors, not as survey truth.

## 6. Identity

Per PPS-0001 §5.4 (Stable Identity): identities are UUIDs, immutable, and independent of memory layout, execution order, and process lifetime.

- **ArtifactID** = `artifact_uuid` (UUIDv4, random) — the current artifact-store instance id (`artifact-format.md` §3). It identifies an artifact *instance*, not its content.
- **FrameID** = `UUIDv5(namespace, name)` — deterministic.
- **ObservationID** = `UUIDv5(namespace, name)` — deterministic.

Canonical name (identical for frame and observation, disambiguated by namespace prefix):

```
name := <prefix> "|" <sensor_id> "|" <timestamp_ns> "|" <content_hash>
```

- `prefix` = `frame` for FrameID, `observation` for ObservationID.
- UUID serialization: lowercase, hyphenated. `timestamp_ns`: decimal. `content_hash`: lowercase hex SHA-256.
- Namespace: a single reserved platform namespace UUID, fixed in the identity implementation (`core/scene/identity.h`), identical across runs and installations.

Consequences:

- The same sensor, capture time, and byte content always map to the same FrameID/ObservationID — stable across re-imports, machines, and processes.
- Two captures of the same bytes at a different time or by a different sensor produce different IDs — the dedup case of §13.
- Reproducibility of identity is therefore content-derived, matching PPS-0001 §6.3.

## 7. Capture Session determination

All spatial entities MUST exist inside a Capture Session or a derived Spatial Context (PPS-0001 §5.2).

- **Explicit:** if the caller provides `session_id`, the importer resolves it and uses it. It must exist.
- **Implicit:** otherwise the importer **creates one session per import batch** (`capture_sessions`: project, name, `source_uri`, started/ended timestamps, provenance). `session_id` is a random UUIDv4 assigned at batch creation.
- Every image written during the batch records the batch session; a batch never spans sessions, and an image never joins a session implicitly after the fact.
- Session identity is instance identity (a grouping artifact), not content identity — it is not part of the v5 name and does not affect FrameID/ObservationID.

## 8. Storage and ImageArtifact creation

- The importer calls the artifact store's `Put(payload, manifest)` (`core/artifacts/artifact_store.h`). The payload is the **exact original bytes** — no transcode, no re-encode, no strip of EXIF. Byte-for-byte fidelity is a requirement (§9).
- Layout follows `artifact-format.md` §2: payload at `artifacts/cas/<hash[0:2]>/<hash>`, manifest at `artifacts/<artifact_uuid>/manifest.json`.
- Write is atomic (temp + `fsync` + rename, `artifact-format.md` §4). The manifest is written last.
- Manifest envelope (draft `image.schema.json`, inline in §16, file at implementation):

| Field | Value |
|---|---|
| `artifact_uuid` | random UUIDv4 |
| `content_hash` | SHA-256 hex of payload |
| `type` | `"image"` |
| `schema_version` | `1` |
| `producer` | `{ id: "image-import", version, git_commit }` |
| `input_artifact_hashes` | `[]` (import has no upstream artifacts) |
| `configuration_hash` | SHA-256 of canonical importer config |
| `creation_timestamp` | ISO-8601 UTC |
| `coordinate_frame` | `"image"` (raw image space) |
| `unit` | `"meter"` (default; not applicable to raw pixels) |
| `file_size`, `mime_type`, `validation_status` | from the bytes; `"valid"` after checks |
| `width`, `height`, `pixel_format` | image geometry (import schema extension) |

## 9. Checksum and content identity

- **SHA-256 of every byte** is computed at ingestion, over exactly the payload bytes (`artifact-format.md` §1). This is the content identity.
- On write, the store re-verifies `SHA-256(payload) == content_hash`; a mismatch is a corruption failure and the bytes are never published.
- On read, the store re-verifies before handing bytes out and quarantines on mismatch (`artifact-format.md` §6).
- Two writes of identical bytes deduplicate to a **single CAS payload** (ADR-010). Dedup is at the payload layer only (§13).

## 10. Coordinate and time metadata

- **Time.** Capture time is normalized to `TimestampNs` in the platform domain through the time-domain graph (`sensor-model.md` §4). An unresolvable timestamp is a validation error — never a silent guess.
- **Position.** GPS/pose values from EXIF enter as `pose_prior` on the observation (ADR-006), never as authoritative geometry. No `pose_ref` is set at import.
- **Frame.** The manifest's `coordinate_frame` is `"image"` (raw image space). A frame for the image data (camera frame, `sensor-model.md` §2) is resolved by downstream stages against sensor calibration.

## 11. Provenance

Every artifact and every observation records full provenance (PPS-0001 §5.7, ADR-015):

- **ImageArtifact:** `producer` (importer id + version + git commit), `configuration_hash`, `creation_timestamp`, and the original `source_uri` + source mtime recorded in the artifact's provenance block (`artifact-format.md` §7).
- **Frame / ImageObservation:** `source` (`image-import` + version + git commit) and `provenance` (input config hash). The observation's `artifact_ref` links to the ImageArtifact, completing the chain bytes → artifact → observation → frame.

## 12. Validation and errors

Error taxonomy (machine-readable codes on the failed task's diagnostic):

| Code | Condition |
|---|---|
| `unreadable` | I/O error reading the file |
| `corrupt` | bytes present but header/decode fails (bad magic, truncated) |
| `unsupported_format` | MIME not in the allowlist (§14) |
| `missing_exif` | a required metadata field is unresolvable |
| `sensor_unresolved` | no `sensor_id` resolvable from capture context |
| `timestamp_unresolvable` | capture time cannot resolve to `TimestampNs` through the domain graph |
| `validation_error` | produced record fails schema validation |

Rules:

- **Per-file failure, not batch failure.** An unreadable/corrupt/unsupported file is a `failed` import task with a diagnostic; the stage persists all successfully imported observations and continues (ADR-014, `reconstruction-pipeline.md` §2.1).
- A failed file is never partially written: no frame, no observation, no artifact unless the entire file completed.
- Uncalibrated observations are imported with a warning (`calibration_unresolved` → flagged `uncalibrated`), not failed (`sensor-model.md` §3.1).

## 13. Duplicate handling

Two distinct cases:

1. **Content-identical, context-identical** (same `content_hash`, `sensor_id`, `timestamp_ns` — i.e. re-importing the identical file with identical capture metadata): import is **idempotent**. The importer returns the existing FrameID/ObservationID, reuses the CAS payload, and creates **no** new artifact instance or observation. Existing artifacts are never mutated (PPS-0001 §5.3).
2. **Content-identical, context-different** (same bytes captured at a different time, or by a different sensor): the CAS payload deduplicates to one stored blob, but the importer creates a **new artifact instance** (new random `artifact_uuid`) and a **new Frame + ImageObservation** (v5 IDs differ because the name differs). Each instance carries its own full provenance.

The dedup decision in case 2 exists precisely to support re-use of the same content in different capture contexts while keeping the CAS single-copy (ADR-010).

## 14. Unsupported formats

- A file whose MIME/magic is not in the allowlist is rejected with `unsupported_format`, the rejection is recorded with provenance (path, mime, importer version), and the stage continues.
- There is **no silent transcode and no silent conversion** of an unsupported format into a supported one (PPS-0001 §5.2).
- **RAW policy:** RAW families (CR2, NEF, ARW, DNG) are **supported for import** but stored as opaque byte payloads with no decode and no pixel interpretation. EXIF/sidecar metadata is captured as available. Decoding is a future capability, not part of P2.1.

## 15. Reproducibility

- **Cache key** = hash of (content bytes SHA-256, canonical importer config JSON, importer version, git commit) (ADR-020).
- **Determinism:** the importer is deterministic (PPS-0001 §6.3): no dependency on filesystem iteration order (input processed in sorted source-URI order), no wall-clock values in outputs, stable field ordering in canonical JSON. `creation_timestamp` is wall-clock by design and is excluded from the cache key.
- **Reproducibility level.** Two runs over identical inputs with identical code and configuration produce: identical content hashes; identical FrameID/ObservationID (v5); identical observation/frame sets (same count, same field values); identical scene outcome up to instance identity. Per-run UUIDv4 values (`session_id`, `artifact_uuid`, scene version IDs) differ by design — they are instance identity, not content.

## 16. Acceptance criteria

Checklist for the P2.1 implementation:

1. **Counts.** Importing N supported files produces N ImageArtifacts, N Frames, N ImageObservations; every `artifact_ref` resolves; every CAS payload hash equals `SHA-256` of the original bytes.
2. **Fidelity.** Original bytes are stored unaltered (byte-for-byte comparison passes).
3. **Identity.** FrameID/ObservationID are stable v5 values; the same tuple re-imports to the same IDs.
4. **Session.** Every record belongs to the explicit or batch session; PPS-0001 §5.2 holds.
5. **Dedup.** Two identical-byte files in different contexts → one CAS payload, two artifact instances, two observations. Re-import of an identical tuple → no new instances.
6. **Failures.** Corrupt and unsupported files produce `failed` tasks with the correct code; other files still succeed; the stage is not aborted.
7. **Calibration.** Observations with a calibration valid at capture time resolve it; otherwise they are flagged `uncalibrated`.
8. **Schema.** All ImageArtifacts validate against `image.schema.json`; all frames/observations validate against the scene model schemas/proto.
9. **Provenance.** Every artifact has producer + config hash + creation timestamp; every observation has source + provenance.
10. **Reproducibility.** Re-running the same import reproduces §15's level exactly.
11. **Boundary.** No vendor type or foreign field leaks into the Canonical Spatial Model; the importer touches only `importers/images/**` and canonical records.

### Inline draft: `schemas/json/image.schema.json` (created at P2.1 implementation, under RFC-0002)

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "image.schema.json",
  "title": "ImageArtifact metadata (P2.1)",
  "type": "object",
  "required": [
    "artifact_uuid", "content_hash", "type", "schema_version", "producer",
    "configuration_hash", "creation_timestamp", "coordinate_frame", "unit",
    "file_size", "mime_type", "validation_status", "width", "height", "pixel_format"
  ],
  "properties": {
    "artifact_uuid": { "type": "string", "format": "uuid" },
    "content_hash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
    "type": { "const": "image" },
    "schema_version": { "const": 1 },
    "producer": {
      "type": "object",
      "required": ["id", "version", "git_commit"],
      "properties": {
        "id": { "type": "string" },
        "version": { "type": "string" },
        "git_commit": { "type": "string" }
      }
    },
    "input_artifact_hashes": { "type": "array", "items": { "type": "string" } },
    "configuration_hash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
    "creation_timestamp": { "type": "string", "format": "date-time" },
    "coordinate_frame": { "type": "string" },
    "unit": { "type": "string" },
    "file_size": { "type": "integer", "minimum": 0 },
    "mime_type": { "type": "string" },
    "validation_status": { "enum": ["valid", "degraded", "unverified"] },
    "width": { "type": "integer", "minimum": 1 },
    "height": { "type": "integer", "minimum": 1 },
    "pixel_format": { "type": "string" },
    "exif": { "type": "object" },
    "band": { "type": "string" }
  }
}
```

## 17. References

- PPS-0001 (§5.1–§5.9, §6.3, §8) — `docs/PPS-0001-platform-principles.md`
- ADR-004 (COLMAP as SfM backend), ADR-006 (AI outputs are priors), ADR-007 (coordinate conventions), ADR-009/010 (SQLite + CAS separation, content addressing), ADR-014 (error handling), ADR-015 (logging and provenance), ADR-020 (task cache), ADR-021 (mock adapters), ADR-023/024 (Scene, Observation Graph), ADR-033 (immutable scene versioning), ADR-034 (capability architecture), ADR-035 (Scene Query API)
- `docs/specifications/reconstruction-pipeline.md` §2.1 (Import stage)
- `docs/specifications/scene-model.md` §4.2, §4.4 (Frame, Observations)
- `docs/specifications/sensor-model.md` §2, §3.1, §4 (camera, calibration, time domains)
- `docs/specifications/artifact-format.md` (§2–§7)
- `docs/development/adding-adapter.md` (capability-driven adapters)
