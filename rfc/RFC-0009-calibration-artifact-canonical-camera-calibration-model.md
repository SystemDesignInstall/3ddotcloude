# RFC-0009 — Calibration Artifact & Canonical Camera Calibration Model

- **Status:** draft
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-13
- **Supersedes:** none
- **Depends on:** RFC-0002, RFC-0005, RFC-0006, RFC-0008, PPS-0001, ADR-006, ADR-007, ADR-010, ADR-020, ADR-024, ADR-033, ADR-034, ADR-035, ADR-038, ADR-039
- **Protected surfaces touched:** Artifact Format (`core/artifacts/**` — new `calibration` artifact type), Capability API / schemas (`schemas/json/calibration.schema.json` — canonical camera-calibration model: intrinsic-model taxonomy extension), PPS-0001 (§5.3 artifact types, §5.8 canonical names — `calibration` / `CalibrationArtifact`), Scene read (`core/scene/query/**` — calibration→artifact materialization through the accepted `SceneQuery` boundary). Capability taxonomy is unchanged: no new capability is introduced.

## Summary

RFC-0009 ratifies the **CalibrationArtifact** — a general, backend-independent canonical spatial artifact carrying a camera calibration document — and the **Canonical Camera Calibration Model** that its payload conforms to. `calibration` is added to the canonical artifact types (PPS-0001 §5.3) and `CalibrationArtifact` to the canonical names (PPS-0001 §5.8); `schemas/json/calibration.schema.json` becomes the normative payload contract, extended additively so the COLMAP camera-model set maps onto it. The artifact is the immutable, CAS-addressed, provenance-rich payload that **backs** the versioned scene `Calibration` record (sensor-model.md §3.1); it never replaces it. Reconstruction stages may receive an optional CalibrationArtifact content hash in `TaskRequest.input_refs`; **calibration data never enters `config_json`**, which remains algorithm configuration only (RFC-0008 §9). This RFC is a deliberate governance precedent: the artifact is created as a general spatial type consumed by every backend (COLMAP, Open3D, GTSAM, VGGT, LingBot, future adapters) — **never** a vendor-scoped `COLMAPCalibrationArtifact`.

## Motivation

1. **The first production reconstruction path needs a canonical calibration input.** RFC-0008 §7 names intrinsics "from `calibration.schema.json`" as canonical input but specifies no transport. The C1 execution contract requires the COLMAP worker to receive calibration through `input_refs` as an immutable CAS artifact (RFC-0008 §10, ADR-038) while keeping `config_json` pure execution configuration (RFC-0008 §9). Today no such artifact exists.

2. **Calibration is a spatial observation, not execution configuration.** PPS-0001 §2, principle 2: calibration is versioned independently and lives on `Calibration` records, never inside image metadata. PPS-0001 §5.2: every derived spatial entity exists as an artifact with provenance. Calibration is exactly such an entity: a permanent, immutable, provenance-rich record of what a sensor was. Baking it into `config_json` would (a) mix spatial measurement data into the ADR-020 cache-key configuration digest, (b) make calibration provenance unaddressable independently of a run, and (c) conflate two distinct classes of information. Calibration is **data**; `config_json` is **algorithm settings**.

3. **The canonical camera-calibration model is under-specified for real backends.** `calibration.schema.json` defines `intrinsic_model` with `["opencv", "pinhole", "opencv_fisheye", "omnidirectional", "opengl", "custom"]` and a `distortion` model enum. RFC-0008 §6 ratifies that "COLMAP camera models map onto the `calibration.schema.json` taxonomy (`fov`, `brown_conrady`, ...)" but the current schema does **not** enumerate `fov` as an intrinsic model, so the ratified mapping has no concrete target. The taxonomy must be extended so the mapping is well-defined.

4. **Governance gap.** PPS-0001 §5.3 lists `image, feature, match, sparse_model, dense_model, mesh, texture, quality_report`; `calibration` is absent. RFC-0008 **activated** `sparse_model` (already reserved); it cannot create a new type. Per CONSTITUTION §2 (Artifact Format is a protected surface) and PPS-0001 §8 ("a ratified RFC formally amends this specification"), a new RFC is the only correct vehicle. RFC-0009 is that vehicle; it is the same governance path RFC-0007 used for the `feature` type.

## Design

### 1. Decision

A **CalibrationArtifact** is a canonical artifact of type `calibration` whose payload is a JSON document conforming to `schemas/json/calibration.schema.json` (the Canonical Camera Calibration Model), with a standard `ArtifactManifest` (producer, `input_artifact_hashes`, `configuration_hash`, `coordinate_frame`, `unit`, `schema_version: 1`). It is a **general spatial artifact**: any backend that consumes camera intrinsics consumes CalibrationArtifacts; any solver or calibrator produces them. It is never named after a vendor.

```
CalibrationArtifact
       │
       ├── COLMAP        (matching / mapping / BA input)
       ├── Open3D        (registration / ICP input)
       ├── GTSAM         (pose/loop-closure factors input)
       ├── VGGT          (prior input, ADR-006)
       └── LingBot       (reserved future backend)
```

### 2. Scope

**In scope:**

- `calibration` artifact type added to PPS-0001 §5.3; `CalibrationArtifact` added to §5.8 canonical names.
- Canonical Camera Calibration Model: additive extension of `calibration.schema.json` intrinsic-model taxonomy so the COLMAP camera-model set maps onto it; normative COLMAP ↔ canonical mapping table (§5).
- CalibrationArtifact lifecycle: production, provenance, immutability (ADR-010), relationship to the versioned scene `Calibration` record (§4).
- Canonical input contract: optional CalibrationArtifact content hash in `TaskRequest.input_refs` for calibration-consuming stages; `config_json` stays algorithm-only (§6).

**Out of scope (later RFCs / milestones):** calibration *solving* (intrinsics/extrinsics estimation, rig calibration, time-offset estimation — deferred per sensor-model.md §5 / ADR-031); `feature.schema.json` changes (governed by RFC-0008 §6, landed with the COLMAP adapter); any AI calibration backend; the COLMAP adapter implementation itself (C1, after this RFC).

### 3. Why an artifact, not only a scene record

The scene `Calibration` record (sensor-model.md §3.1) is the **versioned, time-valid pointer layer**: `calibration_id` + version + validity interval, resolved at capture time via `SceneQuery::ResolveCalibrationAt`. It is the runtime authority. The CalibrationArtifact is the **immutable, content-addressed payload** behind it. The two coexist:

- The artifact is produced once, addressed by content, never changed (PPS-0001 §5.3, ADR-010); identical calibration content deduplicates in the CAS.
- Calibration is carried across the worker boundary **as a content hash** (RFC-0003 §5.9, ADR-038) — the worker materializes the payload from the CAS into its isolated workspace and never touches scene rows or the metadata DB.
- The scene record's `source` already records "contributing artifact hashes" (sensor-model.md §3.1); the CalibrationArtifact is that contributing artifact. A new scene record (new version / new validity interval) references a new artifact instance as needed; past reconstructions keep the artifact valid at capture time.
- CalibrationArtifact provenance is **independently addressable** without any processing run: `ArtifactManifest.producer`, `input_artifact_hashes` (e.g. the image artifacts used for self-calibration, or empty for a factory calibration), `configuration_hash` (the calibration computation config when solved in-platform, or the source's declared digest), `creation_timestamp`, `coordinate_frame`, `unit`.

### 4. Canonical Camera Calibration Model (payload)

The payload is a JSON document conforming to `calibration.schema.json`. Its normative content per the user-facing contract:

| Concept | Field (calibration.schema.json) | Notes |
|---|---|---|
| Camera model | `intrinsic_model` | canonical vocabulary (§5) |
| Intrinsics | `intrinsics` | e.g. `{fx, fy, cx, cy}` in pixels |
| Distortion | `distortion.model` + `distortion.coefficients` | `none` / `opencv_radial` / `opencv_fisheye` / `equirectangular` / `custom` |
| Sensor association | `sensor_id` | owning sensor; per-sensor in a rig, or one artifact per camera |
| Calibration provenance | `source` + manifest `producer` | factory / import / self-calibration |
| Uncertainty | `uncertainty` | optional per-parameter covariance (ADR-025) |
| Coordinate-frame semantics | `extrinsics.{from_frame,to_frame,rot_xyzw,t_xyz}` | strict transform (ADR-007); absent for intrinsic-only |
| Validity | `valid_from_ns` / `valid_to_ns` | half-open interval; open-ended allowed |
| Version | `version`, `calibration_time_ns` | monotonic; production time, not validity |

Manifest metadata: `type: "calibration"`, `schema_version: 1`, `coordinate_frame` per the extrinsic semantics (rig/world when extrinsics present, empty for intrinsic-only), `unit: "meter"` for translations and `pixels` for intrinsics (PPS-0001 §5.5).

### 5. Canonical intrinsic-model taxonomy (additive schema extension)

`calibration.schema.json` `intrinsic_model` gains **`"fov"`** (additive; existing values unchanged). The ratified canonical vocabulary and the COLMAP mapping:

| Canonical `intrinsic_model` | Parameters | COLMAP camera model(s) |
|---|---|---|
| `pinhole` | fx, fy, cx, cy | `SIMPLE_PINHOLE` (fy=fx), `PINHOLE` |
| `opencv` | fx, fy, cx, cy, k1, k2, p1, p2 | `OPENCV`; `SIMPLE_RADIAL` / `RADIAL` as subsets |
| `fov` (new) | fx, fy, cx, cy, omega | `FOV` |
| `opencv_fisheye` | fx, fy, cx, cy, k1..k4 | `OPENCV_FISHEYE`; `SIMPLE_RADIAL_FISHEYE` / `RADIAL_FISHEYE` subsets |
| `omnidirectional` | model-specific | panoramic / 360° sensors |
| `opengl` | model-specific | retained |
| `custom` | opaque params | `THIN_PRISM_FISHEYE` and any model not otherwise classified (params preserved, semantics opaque) |

The mapping table is the contract RFC-0008 §6 refers to ("COLMAP camera models map onto the `calibration.schema.json` taxonomy"). The **conversion itself** lives in the COLMAP adapter (`colmap_converter`, C1) — this RFC ratifies the vocabulary, not the adapter. Extending the vocabulary further (e.g. a new camera model) is an additive schema change governed by RFC-0005's Schema Evolution Policy and this RFC's change-control (§9).

### 6. Canonical input contract

For every stage that consumes camera intrinsics (`feature_extraction`, `sparse_reconstruction`, `bundle_adjustment`):

```
TaskRequest.input_refs
    ├── ImageArtifact hashes          (RFC-0008 §7)
    └── CalibrationArtifact hash      (optional, this RFC)
TaskRequest.config_json
    └── algorithm settings ONLY       (SIFT/matcher/mapper params, threads, seed)
```

- CalibrationArtifact hash, when present, enters `input_artifact_hashes` and the ADR-020 cache key as an **input**, independent of `configuration_hash = Sha256Hex(config_json)`. Equal calibration content + equal config + equal images ⇒ cache replay (ADR-020, AC-8).
- **Absence is not a failure.** COLMAP self-calibrates from available image metadata (EXIF); a stage with no CalibrationArtifact runs in self-calibration mode, and any solved intrinsics enter results through the normal provenance path. This keeps the C1 vertical slice unblocked by calibration data availability.
- The session/CLI layer resolves the scene record (`SceneQuery::ResolveCalibrationAt`), serializes it to the payload, writes the CalibrationArtifact to the CAS, and passes its hash in `input_refs` — mirroring the existing `WriteFeatureArtifactPayload` / session-layer pattern (RFC-0007 §8). The worker consumes only the CAS payload (ADR-038); it never reads `calibrations` rows.
- `config_json` must **never** contain `fx`/`fy`/`cx`/`cy` or any spatial measurement. A calibration value in the configuration surface is a contract violation rejected by validation.

### 7. Provenance

Every CalibrationArtifact carries (RFC-0008 §8; `ArtifactManifest`): `producer {id, version, git_commit}` (solver/calibrator or session layer), `input_artifact_hashes` (source artifacts), `configuration_hash`, `creation_timestamp`, `coordinate_frame`, `unit`, `validation_status`. Provenance is never discarded (CONSTITUTION principle 12). When a calibration is later used as a **prior** (e.g. an AI-estimated focal prior, ADR-006), it enters exclusively as a validated input artifact to a downstream stage and is never authoritative geometry.

### 8. Relationship to RFC-0008 and C1

- RFC-0008 §7's "intrinsics from `calibration.schema.json`" is **made concrete**: the transport is an optional CalibrationArtifact content hash in `input_refs`. This RFC is the governance vehicle RFC-0008 §19/§Open-Questions deferred to.
- The FeatureArtifact/COLMAP flow (Variant A: `ImageArtifact → feature_extraction → FeatureArtifact → COLMAP matcher/mapper/BA → SparseModel`) is **already ratified in RFC-0008 §1 and §7**; this RFC does not alter it.
- C1 (COLMAP execution) consumes CalibrationArtifacts as optional inputs; C1.1 (canonical conversion) and C1.2 (durability) proceed per the approved plan.

### 9. Implementation change-control

- **No implementation before ratification.** This draft does not authorize any change to `spatial-platform`.
- After ratification, implementation proceeds in increments citing `RFC-0009` (`check_constitution --rfc RFC-0009`): (a) PPS-0001 §5.3/§5.8 amendment, (b) `calibration.schema.json` additive taxonomy extension (`fov`), (c) CalibrationArtifact producer/writer + manifest support in `core/artifacts/**`, (d) session-layer wiring and tests. These are pre-requisites for, and separate from, C1 COLMAP implementation (which cites RFC-0008).
- The COLMAP camera-model **conversion** is adapter-side (RFC-0008 §6, PPS-0001 §5.6) and does not require further RFC.

## Compatibility

- **Additive only.** No existing artifact type, schema value, or API changes semantics. `calibration.schema.json` gains `"fov"` in the `intrinsic_model` enum; existing values and existing Calibration scene records are unaffected.
- The versioned scene `Calibration` record (sensor-model.md §3.1) remains the runtime authority; the artifact is additive infrastructure behind it.
- `calibration` is a new PPS-0001 §5.3 type; no existing artifact changes meaning (same additive pattern as RFC-0007's `feature`).
- No new DB migration: `calibrations` and related tables are already schema-resident (migration `0001`); the artifact lives in the CAS (ADR-010).
- Worker protocol is unchanged: CalibrationArtifacts travel as ordinary `input_refs` content hashes.

## Alternatives

- **Calibration in `config_json` (C1 plan revision 1).** Rejected: conflates spatial measurement data with execution configuration; calibration provenance becomes unaddressable without a run; `configuration_hash` would absorb measurement content. This RFC is the correction.
- **Vendor-scoped artifact (`COLMAPCalibrationArtifact`).** Rejected: violates canonical naming (PPS-0001 §5.8) and the plugin architecture (PPS-0001 §5.6); each backend would fork the model.
- **Extend the scene record only, no artifact.** Rejected: the worker boundary (ADR-038) requires inputs as CAS hashes; a record-only model cannot cross the boundary without scene access or config smuggling.
- **Ratify via RFC-0008 amendment.** Rejected on governance: RFC-0008 is ratified and the lifecycle has no post-ratification amendment state (spatial-rfcs README); a new RFC is the only correct vehicle (CONSTITUTION §2, PPS-0001 §8). The LingBot amendment precedent was pre-ratification.

## Open Questions

- Whether `calibration.schema.json` requires additional additive fields for artifact self-containment (e.g. `artifact_ref`) — resolved at implementation time against the existing schema; additive-only.
- Whether a CalibrationArtifact should ever be **rig-scoped** (one artifact per rig covering multiple sensors) or always per-sensor — per-sensor for C1; rig aggregation is a later milestone decision.

## Impact

- **Governance:** PPS-0001 §5.3 (add `calibration`), §5.8 (add `CalibrationArtifact`); spatial-rfcs RFC index; `check_rfc.py` + `check_constitution --rfc RFC-0009` as implementation gates.
- **Modules (after ratification):** `core/artifacts/**` (CalibrationArtifact producer/writer, `calibration` type), `schemas/json/calibration.schema.json` (`fov` intrinsic model), `core/scene/query/**` (calibration→artifact materialization on the accepted boundary), session/CLI layer (write CalibrationArtifact from `SceneQuery::ResolveCalibrationAt`), `docs/development/colmap-p0-c1-implementation-plan.md` (Revision 3: optional CalibrationArtifact input), `docs/PPS-0001-platform-principles.md`, `docs/project-context-summary.md`.
- **Docs:** sensor-model.md §3.1 (artifact backing of the versioned record), processing-reconstruction-matrix.md (reference), reconstruction-pipeline.md §2 (canonical input contract).
- **Tests:** CalibrationArtifact payload↔schema round-trip; provenance recorded (`producer`, `input_artifact_hashes`, `configuration_hash`); cache key includes calibration hash and excludes it from `configuration_hash`; config-surface rejection test (calibration value in `config_json` rejected); session-layer materialization from a resolved scene record.
- **Acceptance:** a camera with a registered calibration produces a schema-valid CalibrationArtifact in the CAS; a reconstruction stage consuming it receives only content hashes, never scene rows (ADR-038); `config_json` contains no spatial measurement; `check_constitution --rfc RFC-0009` passes.

## References

- CONSTITUTION.md §2 (protected surfaces), §5 (change control)
- `docs/PPS-0001-platform-principles.md` §2, §5.1–§5.8 (§5.3 artifact types, §5.8 canonical names), §8 (compliance / formal amendment)
- RFC-0002 (permanent spatial data model, session context, calibration record), RFC-0005 (AC-8, schema evolution policy), RFC-0006 (import, calibration resolution), RFC-0007 (feature artifact governance precedent, adapter seam), RFC-0008 (§6 COLMAP camera-model mapping, §7 canonical input, §9 configuration hashing, §10 execution surface, §19 change-control)
- `docs/specifications/sensor-model.md` §3.1 (Calibration record, contributing artifact hashes), §5 (M0 scope), `docs/specifications/reconstruction-pipeline.md` §2, `docs/specifications/artifact-format.md`
- `schemas/json/calibration.schema.json`, `schemas/json/artifact-manifest.schema.json`
- ADR-007 (coordinate frames), ADR-010 (CAS), ADR-020 (cache), ADR-024 (observation graph), ADR-025 (uncertainty), ADR-033 (versioning), ADR-034 (capabilities), ADR-035 (Scene Query), ADR-038 (engine boundary), ADR-039 (PPS-0001)
- `docs/development/colmap-p0-c1-implementation-plan.md` (Revision 2, approved; Revision 3 pending this RFC)
