# Reconstruction Pipeline Specification

- **Status:** ratified (P1)
- **References:** ADR-004, ADR-005, ADR-006, ADR-020, ADR-026, ADR-027, ADR-028, ADR-031, ADR-033, `docs/specifications/scene-model.md`, `docs/specifications/task-model.md`, `docs/specifications/adaptive-engine.md`
- **Protected surface:** `core/pipeline/**`, `core/scheduler/**` (Constitution §2)

## 1. Canonical pipeline

The Spatial Platform executes reconstruction through one canonical pipeline:

```
Import → Validate → Optimize → Review → Improve → Finalize
```

The principle: **every stage consumes a scene version and produces a NEW scene version** (ADR-033) plus a set of **immutable, content-addressed artifacts** (SHA-256 CAS, ADR-010). Nothing is ever mutated in place; a stage is a pure function `sceneVersion + artifacts → sceneVersion' + artifacts'` with recorded provenance. This makes every stage **replayable** (the same inputs under the same configuration reproduce the same outputs) and **resumable** (a failed stage restarts from the last persisted state, ADR-020).

- `Optimize` is not a single stage: it is the group of reconstruction stages (`Reconstruct/SfM`, `Dense`, `Mesh`, `Texture`, `Gaussian`) that grow and refine the Geometry Graph (`scene-model.md` §4.5).
- `Review` (the **Quality** stage) is always run; the scheduler may mark it `skipped` for non-final pipeline legs.
- `Improve` is the Adaptive Engine's decision point (see `docs/specifications/adaptive-engine.md`): it selects the next recipe from observed quality, or declares the scene ready for `Finalize`.
- **Workers are separate processes** (ADR-028): a stage runs in a worker process, communicates via artifacts and scheduler state, and never holds locks on the project while running. The scheduler tracks a **DAG** of tasks with states `pending, running, succeeded, failed, cancelled, skipped` (`task-model.md`).

## 2. Stage reference

Each stage is specified as: purpose, inputs, outputs, adapters (by **Capability**), quality criteria, failure semantics. Adapters are capability-driven: the platform selects an adapter because it implements a Capability (e.g. `SparseReconstruction`, `DenseStereo`, `BundleAdjustment`, `ICP`, `SurfaceReconstruction`, `Texturing`, `GaussianGeneration`, `LidarOdometry`, `LoopClosure`, `GnssIntegration`), never because of a vendor name.

### 2.1 Import

- **Purpose:** Ingest raw observations into the Scene without altering them.
- **Inputs:** external files — images (JPEG/TIFF/RAW/EXR), video (MP4/MOV), point clouds (E57, LAS/LAZ), ROS bags, vendor formats (Bentley, Pix4D, drone logs, capture-app packages).
- **Outputs:** new scene version `v+1`; `ImageObservation`, `LiDARObservation`, `IMUObservation`, `GNSSObservation`, `DepthObservation` records; imported artifacts (exact byte copies) in the CAS.
- **Adapters:** capability `Import`; importer per source family. The importer **hashes every byte** at ingestion (SHA-256) and writes **provenance** (file path, mtime, importer version + git commit, original checksum). It **never mutates originals** — originals are read-only; the Scene references CAS copies.
- **Quality criteria:** 100% of declared records have resolvable `artifact_ref`; zero observations fail checksum on import; provenance complete on every observation.
- **Failure semantics:** unreadable/corrupt files are reported as `failed` import tasks and do not abort the stage; the stage persists all successfully imported observations and resumes the remaining files on retry.

### 2.2 Validate

- **Purpose:** Prove the imported observations are usable before any optimization runs.
- **Inputs:** imported scene version.
- **Outputs:** new scene version `v+1`; validation report artifact; **SceneCharacteristics** block (extracted for the Adaptive Engine, ADR-027).
- **Adapters:** capability `Validation`. Feature extraction here is the `SceneCharacteristicExtractor` interface (mock in M0, see `adaptive-engine.md` §6).
- **Checks:** data integrity (hashes re-verified), EXIF/intrinsics sanity (focal length present and plausible, distortion model recognized), GNSS sanity (fix type, RTK status, coordinate plausibility, outliers flagged), **overlap estimation** (coarse pairwise overlap for images), and **scene characteristics extraction**: sensor mix, image count/resolution, overlap, motion blur, GNSS/LiDAR availability, texture richness, scene scale, lighting, dynamic content.
- **Quality criteria:** every input observation classified as `ok` / `degraded` / `rejected` with a machine-readable reason; characteristics complete and schema-valid.
- **Failure semantics:** catastrophic failures (unreadable project) fail the stage; per-observation defects degrade the observation, never the stage. `Rejected` observations are excluded from later stages and preserved with provenance.

### 2.3 Reconstruct / SfM

- **Purpose:** Estimate camera poses and a sparse geometry.
- **Inputs:** validated scene version; validated observations (images, optional GNSS/IMU/LiDAR priors).
- **Outputs:** new scene version `v+1`; poses (`WorldFromSensor` with covariance) on the Observation Graph; **Point** geometry elements; `SparseModel` artifact (tracks, point descriptors); BA report.
- **Adapters:** capabilities `SparseReconstruction` + `BundleAdjustment`; **COLMAP is the default adapter** (ADR-004). The Adaptive Engine may select a different adapter or parameters via recipe (ADR-026). Alignment to a survey frame uses `GnssIntegration` / `ICP` capabilities. **AI priors (e.g. VGGT poses) enter only as validated priors on observations, never as authoritative geometry** — through the ADR-006 validation gate.
- **Quality criteria:** reprojection error below recipe threshold; pose covariance finite and well-conditioned; coverage of observations ≥ recipe minimum; component graph connected (or explicitly partitioned).
- **Failure semantics:** degenerate configurations (no overlap, insufficient images) produce a `failed` SfM task with a diagnostic report; partial models are preserved as artifacts and the stage may resume from a persisted subset (ADR-020).

### 2.4 Dense

- **Purpose:** densify geometry from estimated poses.
- **Inputs:** SfM scene version (poses + sparse Points).
- **Outputs:** new scene version `v+1`; per-image **depth maps**; dense **Point** geometry elements (with optional normal/color/confidence channels).
- **Adapters:** capability `DenseStereo` (e.g. COLMAP stereo, OpenMVS); depth fusion is a `DenseStereo`-adjacent capability mapping depth maps → Point elements.
- **Quality criteria:** depth completeness ≥ recipe target; density within recipe bounds; points carry uncertainty channels (ADR-025) where available.
- **Failure semantics:** low-texture regions yield lower density, not errors; per-view failures are dropped with provenance; resumable per view-pair.

### 2.5 Mesh

- **Purpose:** produce a surface from dense geometry.
- **Inputs:** dense scene version (Point elements).
- **Outputs:** new scene version `v+1`; **Triangle** elements (indexed surface).
- **Adapters:** capability `SurfaceReconstruction` (e.g. Poisson, Delaunay/ball-pivot; LiDAR-heavy scenes may prefer ICP-refined Poisson).
- **Quality criteria:** non-manifold edge ratio ≤ recipe threshold; holes within budget; mesh bound consistent with scene bounds; frame explicit on every element.
- **Failure semantics:** watertightness failure degrades to a best-effort surface with a flagged report; no partial mesh is discarded without provenance.

### 2.6 Texture

- **Purpose:** assign appearance to the surface.
- **Inputs:** meshed scene version (Triangle elements).
- **Outputs:** new scene version `v+1`; **texture sets** (atlases, UV maps); **PBR channels** (albedo, normal, roughness, metallic, occlusion) as artifacts referenced from elements.
- **Adapters:** capability `Texturing`.
- **Quality criteria:** texel density ≥ recipe target; visible seams ≤ budget; channels schema-valid and referenced.
- **Failure semantics:** per-chunk texturing failures degrade to untextured regions, reported in the quality report; resumable per chunk.

### 2.7 Gaussian

- **Purpose:** produce a splat representation for view-synthesis output types.
- **Inputs:** SfM or Dense scene version (poses + images/depth).
- **Outputs:** new scene version `v+1`; **Gaussian** elements (per-splat mean, covariance/SH, opacity, color), gsplat-compatible export later.
- **Adapters:** capability `GaussianGeneration`.
- **Quality criteria:** training converges within budget; view-synthesis metric (e.g. PSNR) on hold-out views ≥ recipe target.
- **Failure semantics:** optimization divergence fails the stage with diagnostic artifact; checkpoints allow resume (ADR-020).

### 2.8 Quality (Review)

- **Purpose:** measure fitness against the recipe's quality bar and produce the report that drives Improve/Finalize.
- **Inputs:** scene version from the last optimization stage; recipe quality targets.
- **Outputs:** new scene version `v+1` (quality block populated); **QualityReport** artifact (ADR-030 metrics).
- **Adapters:** capability `QualityMeasurement`; quality metadata is **first-class data**, never recomputed-only (`scene-model.md` §4.8).
- **Metrics:** reprojection error, **accuracy** (vs. survey/control points), **completeness** (coverage), baseline/view-angle coverage, expected mesh/gaussian quality.
- **Failure semantics:** measurement failure is always reportable; a stage that cannot produce metrics fails loudly rather than emitting an empty report.

## 3. Recipes

Pipelines are **executed as named, versioned, immutable Recipes** (ADR-026). A user does not assemble stages; they say "reconstruct by recipe **RC High Accuracy**" (or a custom version) and the Adaptive Engine resolves it.

```
Recipe = {
  id, name, version, git_commit, config_schema,
  stages[] : ordered stage list (each: stage kind, capability constraint, quality target),
  params   : typed parameters per stage,
  cache_identity
}
```

- A Recipe is immutable once published: `version` bumps create a new Recipe; the old one remains executable forever.
- `git_commit` pins the exact engine code the recipe was authored against.
- The **Adaptive Engine selects among candidate Recipes** by predicted quality/cost (`adaptive-engine.md` §3). Selection and parameters are logged with full provenance.
- **Implementation is deferred past M0**; only the data model and schema are fixed now (see `schemas/json/recipe.schema.json`).

## 4. Reproducibility and caching

- **Cache key** = hash of (input artifact hashes, recipe identity, producer version, git commit) (ADR-020). Equal keys always reuse the same cached outputs.
- Every stage declares **deterministic** (`true`/`false`) at authoring time; deterministic stages are cached and may be skipped when the cache hits; non-deterministic stages are never replayed from cache without explicit opt-in.
- A cache hit is recorded in provenance as a synthetic producer (recipe + git commit + cache identity), so cached results remain fully auditable.
- CAS deduplication (ADR-010) makes scene versions cheap snapshots: identical artifacts across versions are stored once.

## 5. Failure semantics

- **Per-stage failure modes:** ingestion (`Import`), validation rejection (`Validate`), degenerate/divergent optimization (`Reconstruct`, `Dense`, `Mesh`, `Texture`, `Gaussian`), and measurement failure (`Quality`). Each mode maps to a diagnostic report and a scheduler transition.
- **Retries:** transient failures (worker crash, I/O) retry with backoff; deterministic failures (bad input, bad config) do not retry — they fail with a reproducible report.
- **Cancellation:** any `running`/`pending` task may be cancelled; cancellation is check-pointed so the stage can resume.
- **Partial results are preserved as artifacts** and referenced by the scene version in which they were produced; a failed stage's outputs are never lost, and `Review`/`Improve` can consume them.
- **Resume:** the scheduler persists its DAG state (task states, checkpoint refs) so an interrupted pipeline resumes from the earliest incomplete task (ADR-020, `task-model.md`).

## 6. M0 scope

M0 ships **no pipeline execution** (ADR-031): no scheduler engine, no worker pool, no recipe runner. M0 delivers:

- the pipeline data model and the Recipe schema (fixed now),
- the stage/adaptability **interfaces** (`Stage`, `Adapter`, `SchedulerState`, `RecipeResolver`),
- a **mock pipeline in the SDK/CLI** that walks the stage graph and produces placeholder scene versions and QualityReport stubs, sufficient to exercise scene versioning (ADR-033) and serialization end-to-end.

Real adapters, the worker executor, and recipe execution land after M0; the schemas above do not change.
