# Read-Boundary Review — SceneQuery as the Stable Read Surface for Processing Capabilities

- **Status:** accepted
- **Owner:** Spatial Platform Architecture Board
- **Change-control citations:** ADR-035 (Scene Query API), ADR-038 (Processing Engine boundary), ADR-033 (immutable scene versioning), RFC-0002 (permanent spatial data model, §SceneIndex), RFC-0006 (image import), PPS-0001 §5.2/§5.9 (artifact-centric architecture, algorithm independence)
- **Basis:** `docs/development/p2.1-completion-review.md` (#9: typed scene read surface), `docs/development/p2.2-plan.md` (P2.2 implementation record), `docs/specifications/scene-model.md` §6, `docs/project-context-summary.md` §15
- **Scope:** this review answers one question and does NOT introduce new API proposals. It accepts the current read boundary and records the minimal targeted extensions gated to the start of P2.3.
- **Code changed:** none.
- **Superseded by:** RFC-0007 §4/§5 — gaps (a) `ArtifactHash` and (b) `SceneVersion` reads are implemented in P2.3 C4 (`core/scene/query/scene_query.h:76,81-83`).

## 1. Review question

> Is the current `core/scene/query/SceneQuery` surface sufficient as a **stable read-boundary** for the Processing Engine and the next capability (P2.3 Feature Extraction), under the platform principles that capabilities consume canonical scene data through a typed, read-only surface?

"Stable read-boundary" here means: the single, Constitution-protected (`core/scene/**`), read-only entry point through which any future capability reads scene data — never raw `sqlite3*`, never storage internals. ADR-038 already assigns the execution runtime to `engine/**` (task model, DAG, lifecycle, cache, workers, provenance); the Processing Engine runtime itself was completed in P1 (RFC-0003). What P2.3+ needs is the scene-facing read contract that capabilities run against.

## 2. Intended contract vs current implementation

The full intended `scene.query()` surface is defined by ADR-035 (`ADR-035-scene-query-api.md:14`) and the normative `scene-model.md` §6 (`scene-model.md:147-162`), and extended by RFC-0002 with session/version/statistics accessors and a `SceneIndex` (`findById`, `findBySensor`, `findByTimestamp`, `findSpatial` reserved) (`RFC-0002:270-284`, `RFC-0002:326`).

| Intended channel | Current P2.2 implementation | Status |
|---|---|---|
| `.images()` | `Observations()`, `ObservationsByFrame/Session/Sensor/InTimeRange` → `ImageObservation` (`scene_query.h:62-69`) | Implemented (image subtype only) |
| `.frames()` | `Frames()`, `FramesBySession/Sensor/InTimeRange` → `Frame` with `pose_ref` (`scene_query.h:53-57`) | Implemented |
| `.sensors()` | `ResolveSensor`, `ResolveCalibrationAt(sensor, timestamp)` (half-open validity) (`scene_query.h:45-50`) | Implemented |
| Session → Scene traversal | `FindCaptureSession`, `FindSceneByProject`, `SessionScene` (`scene_query.h:36-42`) | Implemented |
| `.version()` | `SceneQuery::SceneVersion(version_id)` / `SceneVersions(scene_id)` (`scene_query.h:81-83`) | Implemented (RFC-0007 §5) |
| observation → artifact bytes | `SceneQuery::ArtifactHash(artifact_uuid)` → content hash (`scene_query.h:76`); the pipeline's CAS hash | Implemented (RFC-0007 §4) |
| `.lidar()`, `.imu()`, `.gnss()`, `.depth()` | non-image rows exist in `ObservationRow` but are deliberately unqueryable through the typed surface (`scene_query.h:60-61`) | post-M0 (per ADR-035) |
| `.points()`, `.geometry()`, `.visibleFrom()`, `.observedBy()` | geometry-graph joins; no geometry surface yet (ADR-032 base only) | post-M0 (per ADR-035) |
| `.quality()`, `.uncertainty()` | channels (ADR-025); schemas P0, implementation deferred | post-M0 (per ADR-035/ADR-030) |
| spatial-index variants, `findSpatial` | reserved slot only (RFC-0002) | post-M0 |

The P2.2 subset covers exactly what a **P2.3 Feature Extraction** capability consumes: image observations + frames + sensors + calibration, typed and read-only. The deferred channels are precisely the ones P2.3 does not need.

## 3. Consumer audit — who reads scene data today

- **`SceneQuery` (`core/scene/query/scene_query.{h,cpp}`)** is the only typed read surface over scene data. It is read-only by construction (`const MetadataDb&`; `scene_query.h:3-8,33`); read-only safety is proven against an `OpenReadOnly` database (`tests/unit/test_scene_query.cpp`).
- **The Processing Engine is scene-agnostic by design.** No file under `engine/**` includes anything under `core/scene/**`; the engine reads/writes only its own scheduler/manifest/cache tables (migration 0003/0004) and the artifact index/CAS bytes (`scheduler_state_store.cpp`, `execution_manifest.cpp`, `task_cache.cpp`). Pipeline and task inputs are content-addressed artifact refs (SHA-256) only — `ArtifactRef` at `engine/task/task_types.h:16`, `PipelineStage.input/output_artifact_kinds` at `engine/pipeline/pipeline_definition.h:20-21`, external inputs as `std::vector<ArtifactRef>` at `engine/scheduler/scheduler.h:36-38`.
- **The importer** writes scene rows and reads calibration through `ResolveCalibrationAt` (`importers/images/image_importer_main.cpp`); it is a producer, not a query consumer.
- **Tests** (`test_scene_query.cpp`, `test_image_importer.cpp`) are the only other consumers.

Conclusion: the P2.1 review debt #9 ("typed scene read surface is the entry point for the next Capability", `p2.1-completion-review.md:110-114`) is closed by P2.2. The engine ↔ scene boundary is still entirely unwired — which is correct: capabilities must not reach into `MetadataDb` directly, and the bridge belongs to the read surface.

## 4. Read-boundary gaps (assessed for P2.3)

Assessed against the **immediate** consumer, P2.3 Feature Extraction (COLMAP-first adapter, `project-context-summary.md:71`):

| # | Gap | Evidence | Severity for P2.3 | Decision |
|---|---|---|---|---|
| (a) | No resolution `observation.artifact_ref` (artifact **UUID**) → pipeline `ArtifactRef` (content **hash**). Today an imported observation points at an artifact UUID (`image_observation.h:25`), while pipeline inputs are content hashes (`task_types.h:16`); nothing bridges them. | `engine` grep: no `core/scene/**` includes; `pipeline_compiler.cpp` takes CAS hashes only | **Blocker when P2.3 lands** — Feature Extraction must read the image bytes referenced by an observation | **Implemented (RFC-0007 §4, C4)**: `SceneQuery::ArtifactHash(Uuid)` → content hash, backed by `MetadataDb::FindArtifactById`. No schema/engine change. |
| (b) | No `SceneVersion` read accessor. `SceneQuery` returns `Scene` carrying `version_id` (`scene.h:20`) but exposes no version row; the only version accessors are writes (`FindOrCreateScene`/`CreateSceneVersion`, `metadata_db.h:292-302`). | `reconstruction-pipeline.md` — "each stage consumes a scene version and produces a NEW scene version" (ADR-033) | **Required when P2.3/P2.4 stages record versions** | **Implemented (RFC-0007 §5, C4)**: `SceneQuery::SceneVersion(version_id)` / `SceneVersions(scene_id)` returning `SceneVersion`. |
| (c) | Non-image observation views (lidar/imu/gnss/depth) are unrepresentable through the typed surface. | `scene_query.h:60-61`; deliberate | None for P2.3 (image-only) | **post-M0** (per ADR-035); no action now |
| (d) | Worker/thread safety of `SceneQuery` is not explicitly documented (wraps a single `MetadataDb`; SQLite WAL single-writer per ADR-020). | `scene_query.h:3-8` | Low; affects process workers later (IPC already omits input refs, `worker.proto:48-53`) | **Document in the P2.3 work item**; runtime contract change is a separate review. |

Both gaps are **deferral-gated**: they are the minimum a capability needs immediately before P2.3 and must not be built speculatively now. Their design detail is deliberately out of scope for this review (accepted by the review board; see §1). Both are now **implemented** inside the P2.3 work item (RFC-0007 §4/§5, milestone C4).

## 5. Verdict

**The current `SceneQuery` is accepted as the stable read-boundary for the Processing Engine and for P2.3 Feature Extraction.**

- The implemented subset covers P2.3's entire read need (image observations, frames, sensors, calibration) with strict domain types and read-only safety.
- The engine stays scene-agnostic (ADR-038, PPS-0001 §5.9); the boundary between `engine/**` and scene data is the typed surface, and it must remain the only one.
- **No new query channels are to be added now.** `.lidar()/.imu()/.gnss()/.depth()`, `.points()/.geometry()/.visibleFrom()/.observedBy()`, `.quality()/.uncertainty()`, and spatial-index variants remain **post-M0** (ADR-035). Extending them now would be gold-plating ahead of consumers.
- The two extensions (a) artifact-UUID→content-hash resolution and (b) `SceneVersion` read were **P2.3 deferral-gated**: designed and implemented inside the P2.3 work item (RFC-0007 §4/§5, milestone C4), not before it. The worker side of the boundary — workers consume content hashes + CAS only, never `MetadataDb`/`SceneQuery`/SQLite — is enforced by the `check_worker_boundary` gate (`scripts/check_worker_boundary.py`).

## 6. Roadmap correction

`docs/project-context-summary.md:70` labels milestone P2.2 as **Camera Model** (canonical calibration taxonomy in `calibration.schema.json`, `core/calibration`, `core/sensors`). The implemented P2.2 (per `docs/development/p2.2-plan.md`) delivered **Sensor Registration + Scene Query Groundwork** (migration 0006 validity intervals, registration accessors, `core/scene/query/**`, `core/scene/sensor/sensor.h`) — the camera-model calibration taxonomy described in the roadmap was not built. This is recorded so the next capability does not assume the roadmap's P2.2 label matches the delivered code.

The next capability is **P2.3 Feature Extraction** (`project-context-summary.md:71`): `FeatureArtifact`, capability `FeatureExtraction`, COLMAP-first adapter behind `adapters/interfaces/**`.

## 7. Acceptance criteria + validation

- The review adds no code, schema, or engine change. Baseline preserved: full ctest **202/202** in Debug and Release.
- Gates after this document: `check_rfc` → `check_constitution` → `check_domain_types` → `check_schemas` → `check_dependencies` → `check_arch_debt` → ctest Debug/Release, all PASS.
- P2.3 work item included (a) the observation→artifact content-hash read on `SceneQuery` and (b) the `SceneVersion` read accessor — both implemented (RFC-0007 §4/§5) and covered by `tests/unit/test_scene_query.cpp`; no other query channel was expanded.
