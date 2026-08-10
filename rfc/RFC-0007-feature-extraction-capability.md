# RFC-0007 — Feature Extraction Capability

- **Status:** ratified
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-10
- **Supersedes:** none
- **Depends on:** RFC-0002, RFC-0005, RFC-0006, PPS-0001, ADR-004, ADR-010, ADR-020, ADR-021, ADR-024, ADR-031, ADR-033, ADR-034, ADR-035, ADR-038
- **Protected surfaces touched:** Capability API (`schemas/**` — adds `feature_extraction` to `worker-capabilities.schema.json`, new `feature.schema.json`), Scene read-boundary (`core/scene/query/**` — new `SceneQuery::ArtifactHash` and `SceneQuery::SceneVersion/Versions` reads), Adapter interfaces (`adapters/interfaces/**` — new `ProcessingAdapter`/`AdapterDescriptor`, `adapters/colmap/**`), Artifact Format (`core/artifacts/**` — `feature` artifact type), Processing Engine (`engine/**` — `RegisterFeatureExtraction` capability wiring + mock `feature_extract` runner branch)

## Summary

RFC-0007 ratifies the **Feature Extraction** capability for P2.3 (`project-context-summary.md` roadmap): the `feature_extraction` capability is added to the capability taxonomy, and the platform gains the canonical **FeatureArtifact** (`schemas/json/feature.schema.json`, M0 JSON-array representation), the two read-boundary extensions deferred by `read-boundary-review.md` — (a) artifact `UUID → content-hash` resolution and (b) `SceneVersion` reads — a deterministic mock feature-extraction runner (ADR-021), a COLMAP-first adapter descriptor behind `adapters/interfaces/**` with status `planned` and **no linkage** (ADR-031, `THIRD_PARTY.yml`), and a per-frame `feature_sets` record (migration `0001`). The capability is cacheable by construction: the mock output is a pure function of the input image bytes (RFC-0005, AC-8). No new database migrations are introduced.

## Motivation

P2.3 is the first **processing** capability over the Permanent Spatial Data Model (RFC-0002): unlike Import (RFC-0006), which ingests bytes, Feature Extraction reads scene data through the Scene Query boundary and writes a derived artifact back. Three structural gaps block implementation:

1. **The `feature_extraction` capability does not exist in the frozen taxonomy.** `worker-capabilities.schema.json` and `plugin-api.md` §4 enumerate the built-in capabilities; neither names feature extraction, yet the demo worker profile already advertises the string `feature_extraction` (`mock_pipeline_runner.cpp` `DemoWorkerProfile`) and the mock photogrammetry pipeline registers a stage for it (`mock_photogrammetry.cpp`). The taxonomy is Constitution-protected and extensible only by RFC (CONSTITUTION §2; `worker-capabilities.schema.json`).

2. **The read-boundary is accepted but two deferral-gated extensions are now required** (`read-boundary-review.md`, gaps (a) and (b)). A Feature Extraction pipeline consumes **content hashes** (`task_types.h`), while an `ImageObservation` references its image by artifact **UUID** (`image_observation.h`); nothing resolves UUID → hash. And the immutable scene version (ADR-033) has no read path on `SceneQuery`. Both are additive read-only accessors on the already-accepted boundary.

3. **The feature artifact has no contract.** `FeatureArtifact` is a canonical name (PPS-0001 §5.8) and `feature_sets` is schema-resident (migration `0001`), but there is no `feature.schema.json`, no typed `FeatureSet`, and no capability wiring — so a Feature Extraction stage cannot yet produce or record anything.

COLMAP is the long-term default extraction adapter (ADR-004) but is registered `planned` in `THIRD_PARTY.yml` and must not be built or linked under ADR-031; this RFC therefore ratifies the **capability contract + deterministic mock** now and defers the real COLMAP backend behind the adapter interface.

## Design

### 1. Capability taxonomy: add `feature_extraction`

`schemas/json/worker-capabilities.schema.json` adds `"feature_extraction"` to the capabilities enum at the first (alphabetical) position, ahead of `"import"` (same insertion convention as RFC-0006). Semantics: a worker/adapter declaring `feature_extraction` consumes image observations (via `ImageObservation` → artifact content hash) and produces per-image keypoints/descriptors as `feature` artifacts. The engine selects implementations by capability, never by vendor name (ADR-034). The taxonomy remains RFC-versioned (RFC-0002 §6.8).

```json
"enum": [
  "feature_extraction",
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

`plugin-api.md` §4 is synced to include `Import` and `FeatureExtraction` in its built-in list (the section currently predates RFC-0006 and omits `import`).

### 2. `feature.schema.json` — M0 JSON-array representation

Formalized at `schemas/json/feature.schema.json` (draft 2020-12). The artifact **payload** is a self-describing JSON document (the same "payload is JSON → schema describes the payload" pattern as `quality-report.schema.json`); the manifest is the generic `ArtifactManifest` with `type: "feature"` and `schema_version: 1`.

Required payload fields: `detector`, `descriptor_type`, `count`, `keypoints`, `descriptors`, `schema_version` (`const 1`). `keypoints` is an array of `{x, y, size?, angle?, response?}` objects (image-space pixel coordinates); `descriptors` is an array of numeric arrays, one row per keypoint. The producer guarantees `count == keypoints.length == descriptors.length` (not expressible in JSON Schema; enforced by the producer and tests). `additionalProperties` is implicit-true (additive-only under the RFC-0005 Schema Evolution Policy).

The M0 representation is a **JSON array format**. A packed/binary encoding (fixed-width float blocks) is reserved for the real COLMAP adapter, which will declare a future `schema_version`; M0 consumers never parse it.

Initial canonical vocabulary (extensible): `detector: "mock"` for the M0 deterministic runner; `descriptor_type: "mock_16"` (16-dim float rows). COLMAP's SIFT names are ratified only when the COLMAP adapter lands (post-M0, behind the interface).

### 3. FeatureArtifact + `feature_sets` — per-frame, immutable

One **FeatureArtifact per frame** (one image → one feature set), matching the per-frame `feature_sets` row and COLMAP's per-image keypoint files. The chain is `image bytes → ImageArtifact → Frame → ImageObservation → feature bytes → FeatureArtifact → FeatureSet`, where `feature_sets.artifact_ref` stores the FeatureArtifact's `artifact_uuid` (canonical string, same convention as `observations.artifact_ref`).

`core/storage/metadata_db.*` gains the first typed accessors for the schema-resident `feature_sets` table (migration `0001`, **no new migrations**): `InsertFeatureSet` (write) and `FindFeatureSetsByFrame`/`FindFeatureSetsByScene` (read). A `FeatureSet` domain type (per-frame: `feature_set_id`, `frame_id`, `detector`, `descriptor_type`, `count`, `artifact_ref` UUID) lives under `core/scene/**`. Feature artifacts are immutable once written (PPS-0001 §5.3, ADR-024).

### 4. Read-boundary extension (a): artifact UUID → content hash

`read-boundary-review.md` gap (a). `MetadataDb` gains `FindArtifactById(Uuid) → std::optional<ArtifactIndexRow>` (reverse lookup of the existing `artifact_index` table, which already stores both `artifact_id` and `content_hash`). `SceneQuery` gains `ArtifactHash(Uuid artifact_uuid) → std::optional<std::string>` (content hash) as a **read-only** accessor on the accepted boundary. No new read channels are created; the engine remains scene-agnostic (ADR-038) and receives content hashes only.

### 5. Read-boundary extension (b): SceneVersion reads

`read-boundary-review.md` gap (b). `MetadataDb` gains `FindSceneVersion(version_id)` and `FindSceneVersionsByScene(scene_id)`; `SceneQuery` gains typed `SceneVersion(version_id)` and `SceneVersions(scene_id)` reads over the `scene_versions` table (migration `0001`). Read-only; ADR-033 immutability is preserved.

### 6. Capability wiring: `RegisterFeatureExtraction` + mock runner

`engine/pipeline/` gains `RegisterFeatureExtraction(PipelineRegistry&)` following the `RegisterMockPhotogrammetry` pattern (`mock_photogrammetry.{h,cpp}`): a single-stage chain `feature_extract` (stage task type `feature_extract`, capability `feature_extraction`, artifact kinds `{image} → {feature}`), registered at the same call sites (`cli/main.cpp`, mock-pipeline e2e tests).

The mock runner (`engine/workers/mock_pipeline_runner.cpp`) gains a `feature_extract` branch that — unlike the existing generic branch, which builds its payload from metadata only — genuinely reads the input: `store.Get(request.input_refs[0])` returns the image bytes, and the output is a **deterministic** feature payload derived from those bytes (fixed-size mock keypoint grid seeded by the input hash), so identical input bytes yield identical output bytes and the task cache replays correctly (ADR-020, AC-8). The produced artifact uses `type: "feature"`, `mime_type: "application/json"`, `coordinate_frame: "image"`, `unit: "pixels"`.

`DemoWorkerProfile` already advertises `feature_extraction`; after §1 its declaration becomes schema-valid (no change needed).

### 7. Adapter interfaces: COLMAP-first, mock-only

- `adapters/interfaces/**` (Constitution-protected, CONSTITUTION §2) gains the minimal **`ProcessingAdapter`** interface and **`AdapterDescriptor`** type (ADR-034; `plugin-api.md` §7 M0 scope).
- `adapters/colmap/**` carries the COLMAP-first descriptor: capability `feature_extraction`, status `planned`, `license_reference` pointing at `THIRD_PARTY.yml` (COLMAP, BSD-3). The M0 implementation is an in-process **deterministic mock** (ADR-021) implementing the interface; COLMAP is **not built, linked, or bundled** (ADR-031). The real COLMAP backend is a later increment behind the same interface.
- **Explicitly deferred:** `PluginManager` (descriptor-directory loading), dynamic plugin loading, and `core/plugin/**` (ADR-034 M0 deferral list). The engine selects the capability via `ResourceProfile`/pipeline registration as today.

### 8. CLI bridge

`spatial run feature-extraction --session <id>` wires the full vertical: `SceneQuery::ObservationsBySession` → `SceneQuery::ArtifactHash(observation.artifact_ref)` → `store.Get(hash)` (inside the mock runner) → FeatureArtifact → `InsertFeatureSet`. This is the first real consumer of the (a) bridge and the end-to-end proof of a processing capability over the permanent model.

## Compatibility

- **No breaking change.** `feature_extraction` is an enum addition; existing worker declarations are unaffected. `feature.schema.json` is a new schema; no existing artifact type changes meaning. Existing CAS artifacts and manifests remain valid.
- **Additive only.** `FindArtifactById`, `ArtifactHash`, `SceneVersion(s)`, `InsertFeatureSet`, `FindFeatureSetsBy*` extend existing surfaces; no existing API changes semantics.
- Migration `0001` tables (`feature_sets`, `scene_versions`) are used as-is; **no new migrations**.
- `DemoWorkerProfile` remains unchanged and becomes schema-valid.
- The deferred surface (`PluginManager`, `core/plugin/**`, real COLMAP linkage) is untouched by this RFC.

## Alternatives

- **Implement real COLMAP feature extraction in P2.3.** Rejected: COLMAP is `planned` in `THIRD_PARTY.yml` and ADR-031 forbids heavy third-party builds in M0; the capability contract and interface must land first.
- **Per-session aggregate FeatureArtifact.** Rejected: `feature_sets` is per-frame (migration `0001`), COLMAP writes per-image keypoint/descriptor files, and per-frame artifacts keep the (a) bridge and cache keys aligned with observations.
- **Vendor-specific naming (`ColmapFeature`, `OpenMVGFeature`).** Rejected: PPS-0001 §5.8 canonical names.
- **Engine reads SQL directly to resolve artifact hashes.** Rejected: ADR-035 — capabilities consume the typed, read-only `SceneQuery` boundary; the engine stays scene-agnostic (ADR-038).
- **New migrations for feature storage.** Rejected: `feature_sets` already exists in migration `0001`; adding DDL would violate the no-new-migrations constraint.

## Open Questions

- The canonical `descriptor_type` vocabulary is fixed minimally for M0 (`mock_16`) and extended with the COLMAP adapter (post-M0). No M0 consumer depends on SIFT naming.
- Whether a `FeatureExtraction` stage is modeled in `reconstruction-pipeline.md` is a follow-on documentation sync outside this RFC's protected surfaces.

## Impact

- **Modules:** `schemas/json/worker-capabilities.schema.json` (`feature_extraction`), new `schemas/json/feature.schema.json`, `core/storage/metadata_db.{h,cpp}` (`FindArtifactById`, `FindSceneVersion(s)`, `InsertFeatureSet`, `FindFeatureSetsBy*`), `core/scene/query/scene_query.{h,cpp}` (`ArtifactHash`, `SceneVersion/Versions`), new `core/scene/feature/feature_set.h`, `engine/pipeline/feature_extraction.{h,cpp}` (`RegisterFeatureExtraction`), `engine/workers/mock_pipeline_runner.cpp` (`feature_extract` branch with `store.Get`), `adapters/interfaces/**` + `adapters/colmap/**`, `cli/main.cpp` (`spatial run feature-extraction`).
- **Schemas:** `worker-capabilities.schema.json`, new `feature.schema.json`; `plugin-api.md` §4 synced.
- **Docs:** `docs/development/p2.3-plan.md` (implementation plan), `read-boundary-review.md` (gaps (a)/(b) → implemented), `project-context-summary.md` (P2.3 status).
- **Tests:** `tests/unit/test_metadata_db.cpp`, `tests/unit/test_scene_query.cpp` (bridge (a)/(b)), `tests/unit/test_feature_artifact.cpp` (writer + schema conformance), `tests/unit/test_pipeline_compiler.cpp` (single-stage feature pipeline), `tests/unit/test_mock_pipeline_e2e.cpp` (run + cache replay), CLI integration for `spatial run feature-extraction`.
- **Acceptance:** an imported session's observations resolve to content hashes and image bytes; `feature_extract` produces a schema-valid per-frame FeatureArtifact + `feature_sets` row; identical input bytes replay from cache (deterministic output); no new DB migrations; `check_constitution --rfc RFC-0007` passes.

## References

- `docs/development/read-boundary-review.md` (accepted boundary; gaps (a)/(b) implemented by this RFC)
- `docs/development/p2.3-plan.md`, `docs/project-context-summary.md` (P2.3 roadmap)
- RFC-0002 (§6.3, §6.5, §6.8), RFC-0005 (schema evolution, AC-8 cacheability), RFC-0006 (import capability, enum-insertion convention)
- `docs/PPS-0001-platform-principles.md` (§5.3, §5.8, §5.9)
- ADR-004, ADR-010, ADR-020, ADR-021, ADR-024, ADR-031, ADR-033, ADR-034, ADR-035, ADR-038
- CONSTITUTION.md §2 (protected surfaces), §5 (change control)
- `schemas/database/migrations/0001_init.sql`, `schemas/database/schema.sql`
- `THIRD_PARTY.yml` (COLMAP, status `planned`)
