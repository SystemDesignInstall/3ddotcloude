# COLMAP P0/C1 Implementation Plan

- **Status:** Revision 3 (awaiting architecture approval; implementation is LOCKED until approval)
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-13
- **Revision history:** Rev 1 — initial 13-section plan (review 1: CHANGES REQUIRED); Rev 2 — C1/C1.1/C1.2 split, worker-side adapter, engine injection, workspace isolation, resume scoping (approved in principle); **Rev 3 — RFC-0009 RATIFIED (`876d1c4`): calibration is a canonical CalibrationArtifact input (optional content hash in `input_refs`, never `config_json`); pose ownership (worker writes pose data in SparseModel; Session materializes `WorldFromSensor`); resume = scheduler replay only; COLMAP-native continuation deferred**
- **Governing RFC:** RFC-0008 (ratified, `spatial-rfcs` commit `8794bc6`) — Production Reconstruction Backend Architecture; RFC-0009 (ratified, `spatial-rfcs` commit `876d1c4`) — Calibration Artifact & Canonical Camera Calibration Model
- **Companion ADR:** ADR-028 (workflow durability and replay), ADR-020 (scheduler persistence), ADR-011/012 (process workers), ADR-021 (mock), ADR-034 (capabilities), ADR-038 (worker boundary)
- **Scope milestone:** C1 — first production vertical slice: ImageArtifact → Feature Extraction → COLMAP → SparseModel → Camera Poses

**Final production chain (Revision 3):**

```
ImageArtifact
      ↓
Feature Extraction
      ↓
FeatureArtifact / SIFT v2
      ↓
COLMAP Adapter
      ↓
COLMAP matcher / mapper / BA
      ↓
SparseModel
      ↓
CAS
      ↓
Session
      ↓
Observation Graph
```

with optional calibration input:

```
                    ┌────────────────────┐
                    │ CalibrationArtifact│
                    │      optional      │
                    └─────────┬──────────┘
                              │
ImageArtifact ──→ FeatureArtifact ──→ COLMAP ──→ SparseModel
```

CalibrationArtifact infrastructure (RFC-0009 increment) is a **prerequisite** for, and separate from, C1 COLMAP implementation (RFC-0008 increment); C1 consumes CalibrationArtifacts as optional inputs once that increment lands.

**Increments (Rev 2 split, unchanged):**
- **C1 — execution slice (this plan):** ImageArtifact → FeatureArtifact (SIFT `schema_version: 2`) → COLMAP (convert → database → matcher → mapper → BA) → SparseModel; optional CalibrationArtifact input; execution, provenance, cache/replay, failure semantics, tests.
- **C1.1 — canonical conversion:** SparseModel → Session → Observation Graph / `WorldFromSensor` materialization (Session-layer only; separate plan increment).
- **C1.2 — durability:** cache/replay/retry/cancel guarantees hardened to workflow level (ADR-028 deferral boundary).

## 1. Exact Files

### 1.1 New files (implementation)

| File | Purpose |
|---|---|
| `adapters/interfaces/processing_adapter.h` | Activation of the real-adapter seam (RFC-0008 §5; RFC-0007 §7 placeholder). Worker-side `ProcessingAdapter` / `AdapterDescriptor` consumed inside the COLMAP worker; the engine consumes only `WorkerExecutor` (ADR-034). |
| `adapters/interfaces/adapter_descriptor.h` | `AdapterDescriptor`: declared capabilities, input/output schema references, license reference (resolved against `THIRD_PARTY.yml`), runtime/resource profile. |
| `adapters/colmap/colmap_adapter.h` | COLMAP `ProcessingAdapter` implementation: capability declaration (`feature_extraction`, `sparse_reconstruction`, `bundle_adjustment`), config surface, CLI orchestration plan. |
| `adapters/colmap/colmap_config.h` | COLMAP adapter configuration structs + JSON schema handling (detector/descriptor settings, matcher thresholds, mapper options — RFC-0008 §9). |
| `adapters/colmap/colmap_cli.h` | Thin wrapper around the COLMAP CLI (`feature_extractor`, `matcher`, `mapper`, `model_converter`). Building argv + workspace layout. Pure CLI orchestration; no domain types. |
| `adapters/colmap/colmap_converter.h` | COLMAP-format → canonical mapping (RFC-0008 §6, §7): `cameras.bin`, `images.bin`, `points3D.bin` → canonical SparseModel payload (including pose data). COLMAP camera models → canonical `intrinsic_model` via the RFC-0009 §5 mapping. The only file allowed to read COLMAP's native binary formats. |
| `adapters/colmap/sift_encoding.h` | SIFT keypoint/descriptor → packed `feature` encoding (RFC-0008 §6; `schema_version: 2` of `feature.schema.json`). |
| `adapters/colmap/colmap_worker_main.cpp` | COLMAP **process worker** entrypoint: framed protobuf protocol over stdin/stdout (`schemas/protobuf/worker.proto`), `WorkerHello` handshake, task loop. |
| `adapters/colmap/colmap_worker.cpp` | Worker task-body implementation: task → CLI invocation → `TaskArtifactProduced`/`TaskProgress`/`TaskLog` → terminal event. |
| `adapters/colmap/README.md` | Adapter seam checklist status (adding-adapter.md steps 1–9) + build/run/doctor instructions. |
| `schemas/json/sparse-model.schema.json` | **Deferred to P2.5** (RFC-0008 §6). NOT created in C1. |
| `engine/workers/worker_descriptor.h` | Small addition (if required) to bind a `ProcessExecutor` argv to a declared worker (currently hardcoded demo worker). Additive to the executor surface. |

### 1.2 Modified files (additive only)

| File | Change |
|---|---|
| `engine/engine.h` / `engine/engine.cpp` | Composition root: optional `std::unique_ptr<WorkerExecutor>` ctor parameter (default = in-process mock, ADR-021). The engine consumes **only** `WorkerExecutor`; it never sees the adapter. `Engine::Engine` (engine.cpp:41-56) currently builds `InProcessExecutor` unconditionally — the injection seam is the sole change here. Scheduler/registry/compiler untouched. |
| `engine/pipeline/pipeline_registry.h` / `.cpp` | Registration of the COLMAP worker's capability set; no change to the `feature_extraction` pipeline definition semantics. |
| `engine/pipeline/pipeline_definition.h` | No change (capability-based binding already in place). |
| `engine/pipeline/pipeline_compiler.h` | No change (capability resolution against worker profile already implemented). |
| `engine/scheduler/scheduler.h` | No change (executes any `WorkerExecutor`; concurrency stays 1 for C1). |
| `engine/workers/process_executor.h` | No change to the contract; the COLMAP worker is a second `worker_command` argv. Only if the protocol needs an additive extension (RFC-0008 §Compatibility) will the proto/`worker_handle.h` gain fields. |
| `engine/workers/worker_handle.h` | No change unless additive protocol extension is ratified. |
| `schemas/protobuf/worker.proto` | Additive only if C1 requires it (e.g. richer `TaskProgress` substage). No field removals/re-numbering. |
| `schemas/json/feature.schema.json` | SIFT vocabulary + packed encoding: **`schema_version: 2`** (`enum: [1, 2]`, additive per RFC-0005; M0 `schema_version: 1` and its consumers untouched — `test_feature_artifact.cpp` must stay green). Exact schema diff shown before implementation. |
| `schemas/json/calibration.schema.json` | **Additive `fov` intrinsic-model extension** — RFC-0009 §5 (ratified), prerequisite increment citing `RFC-0009`; C1 consumes the extended taxonomy. Existing values unchanged. |
| `core/artifacts/calibration_artifact.h` | **CalibrationArtifact producer/writer** (`type: "calibration"` manifest support) — RFC-0009 increment, prerequisite; C1 consumes artifacts written by the session layer. |
| `core/scene/query/**` | Calibration→artifact materialization on the accepted `SceneQuery` boundary — RFC-0009 increment, prerequisite; session layer serializes the resolved scene record to a CalibrationArtifact. |
| `docs/PPS-0001-platform-principles.md` | §5.3 add `calibration`; §5.8 add `CalibrationArtifact` — RFC-0009 §9(a) amendment, prerequisite. |
| `schemas/json/worker-capabilities.schema.json` | No change in C1; `feature_extraction` / `sparse_reconstruction` / `bundle_adjustment` already in the enum. |
| `core/artifacts/artifact_manifest.h` | No change; `type: "sparse_model"` is a data value, not a code change (PPS-0001 §5.3 activation). |
| `THIRD_PARTY.yml` | COLMAP status `planned` → `integrated` at the commit that proves the worker runs end-to-end. |
| `MODEL_LICENSES.yml` | No change (COLMAP is code, not a model). |
| `docs/development/adding-adapter.md` | Step-9 replaceability result recorded for COLMAP. |
| `docs/development/processing-reconstruction-matrix.md` | Backfill of the executed seam (reference only). |
| `docs/project-context-summary.md` | P2.3/P2.4/P2.5 status: C1 in progress. |
| `tests/unit/CMakeLists.txt` | New test binaries (see §11). |

### 1.3 Gate scripts (run per commit)

`scripts/check_dependencies.py`, `check_schemas.py`, `check_rfc.py`, `check_constitution.py`, `check_domain_types.py`, `check_worker_boundary.py`, `check_arch_debt.py` — all must pass (7/7).

## 2. Existing Interfaces Reused

| Interface | Where | How C1 uses it |
|---|---|---|
| `WorkerExecutor` | `engine/workers/worker_handle.h` | The scheduler already drives any executor. C1 wires a `ProcessExecutor` whose `worker_command` points at the COLMAP worker binary. No scheduler change. |
| `ProcessExecutor` | `engine/workers/process_executor.h` | Spawn + handshake + framed protobuf + heartbeat/cancel. Reused verbatim for the COLMAP worker (RFC-0008 §10). |
| `InProcessExecutor` / `InProcessTaskRunner` / `MockArtifactHash` | `engine/workers/in_process_executor.h` | Test-only parity path (adding-adapter.md Step 9): the mock declares the same capabilities so the whole pipeline runs with no COLMAP installed. |
| `WorkerEvent` / `TaskRequest` | `engine/workers/worker_handle.h` | The COLMAP worker emits `kArtifactProduced`/`kProgress`/`kCompleted`/`kFailed`; scheduler consumes unchanged. |
| `TaskRequest{input_refs, config_json, workspace, ...}` | `engine/workers/worker_handle.h` | Input artifact refs = CAS hashes; config_json = effective configuration (RFC-0008 §9). |
| `TaskGraph` / `TaskMetadata` / `RetryPolicy` / `CancellationPolicy` / `CachePolicy` / `FailurePolicy` | `engine/task/task_types.h` | Six-state lifecycle and policies already drive scheduling; C1 sets `deterministic: true` on COLMAP tasks. |
| `Scheduler` (`Run`, `Resume`, cache-first dispatch) | `engine/scheduler/scheduler.h` | Cache/replay and resume come from the existing scheduler; COLMAP adds nothing new to it in C1. |
| `PipelineRegistry` / `PipelineDefinition` / `PipelineStage` | `engine/pipeline/pipeline_registry.h`, `pipeline_definition.h` | Register the COLMAP-backed pipeline; capability binding already exists. |
| `PipelineCompiler` (`PipelineHash`, `ConfigHash`) | `engine/pipeline/pipeline_compiler.h` | ADR-020 identity hash chain reused as-is for the COLMAP tasks. |
| `Engine` composition root | `engine/engine.h` | Single injection point for the COLMAP worker; mock remains default. |
| `ArtifactStore` / CAS | `core/artifacts/artifact_store.h` | COLMAP outputs written as CAS-addressed immutable artifacts. |
| `ArtifactManifest` | `core/artifacts/artifact_manifest.h` | Producer (`id/version/git_commit`), `input_artifact_hashes`, `configuration_hash`, `coordinate_frame`, `unit`. |
| `SceneQuery` | RFC-0007 | Input set = `SceneQuery::ArtifactHash` for the session's images (RFC-0008 §7). |
| `CalibrationArtifact` (RFC-0009) | `core/artifacts/calibration_artifact.h` (prerequisite increment) | Optional input; content hash travels in `input_refs`; worker materializes the payload from the CAS into its isolated workspace. Absent → COLMAP self-calibration. |
| `calibration.schema.json` | `schemas/json/calibration.schema.json` | Canonical camera-calibration payload contract; `fov` intrinsic model (RFC-0009 §5). Adapter-side only: the worker maps canonical models → COLMAP camera models; Core never sees COLMAP camera types. |
| `feature.schema.json` | `schemas/json/feature.schema.json` | M0 JSON-array payload (`schema_version: 1`, unchanged, mock); SIFT packed encoding lands as **`schema_version: 2`** (`enum: [1, 2]`, additive per RFC-0005) in C1. |

## 3. New Adapter Interfaces

### 3.1 `ProcessingAdapter` (adapters/interfaces/processing_adapter.h)

The activation of the RFC-0007 §7 placeholder (RFC-0008 §5). It is a **worker-side** surface: the engine consumes only `WorkerExecutor`; the adapter is consumed **inside** the COLMAP worker process to plan and execute one task (plugin-api.md §1–2: Core → PluginManager → Plugin → Adapter → Algorithm). Revision 3 removes `WorkerCommand()` from the adapter — process spawning is the worker's concern; the adapter does not construct worker argv.

```cpp
struct AdapterDescriptor {
  std::string adapter_id;            // e.g. "colmap"
  std::string version;
  std::string git_commit;
  std::vector<std::string> capabilities;   // from worker-capabilities.schema.json
  std::string license_ref;           // THIRD_PARTY.yml key
  ResourceProfile profile;           // CPU-first; CUDA optional
  std::vector<std::string> input_artifact_kinds;
  std::vector<std::string> output_artifact_kinds;
};

class ProcessingAdapter {
 public:
  virtual ~ProcessingAdapter() = default;
  virtual AdapterDescriptor Descriptor() const = 0;
  // Validate that the backend is runnable here (doctor step, adding-adapter.md §5).
  virtual bool ValidateEnvironment(std::string& problem) const = 0;
  // Build the execution plan for a worker task (adding-adapter.md §6).
  virtual std::vector<std::string> CreatePlan(const TaskRequest& request) const = 0;
  // Execute the plan inside the worker process; emits canonical artifacts.
  virtual void Execute(const std::vector<std::string>& plan, ResultSink& sink) = 0;
};
```

Constraints (adding-adapter.md §3, RFC-0008 §5, RFC-0009 §8):
- The adapter is the **only** place that reads COLMAP's native data model / binary formats / CLI output.
- It never writes Core, Scene, or CAS structures directly; conversion happens inside the adapter.
- The COLMAP camera-model ↔ canonical mapping (RFC-0009 §5) is **adapter-side** (`colmap_converter`); Core never contains COLMAP camera types (PPS-0001 §5.6 Adapter Isolation).
- No backend includes escape the adapter (enforced by `check_arch_debt.py`).
- Ceres is never exposed through Core or the engine (RFC-0008 §5, ADR-038).

### 3.2 COLMAP worker protocol contract (adapters/colmap/colmap_worker*.cpp)

The COLMAP worker is a process speaking the existing framed protobuf protocol (`schemas/protobuf/worker.proto`) — no new protocol for C1. Contract (RFC-0008 §10):

1. On spawn: emit `WorkerHello` (`protocol_version`, `worker_id`, capabilities = `[feature_extraction, sparse_reconstruction, bundle_adjustment]`, `resources` CPU-first).
2. On `TaskRequest`: `TaskAccepted` → run → `TaskProgress` (substage per CLI tool) → `TaskArtifactProduced` per canonical artifact → `TaskCompleted{outputs}`.
3. On `TaskCancelled`: cooperative stop at the next CLI tool boundary; emit `TaskCancelled`.
4. On determinable failure: `TaskFailed` with `ErrorInfo` (stable code, context, `recoverable`, suggested action) — no exceptions across the boundary (adding-adapter.md §6, ADR-014).
5. Heartbeat liveness; stderr/stdout streamed as `TaskLog`.

## 4. ProcessWorker Contract

| Item | Contract |
|---|---|
| Transport | stdin/stdout, `[u32 LE length][protobuf]` framing; stderr → logs (ADR-011/012). |
| Spawn | `ProcessExecutor(fallback_profile, {worker_binary, workspace}, proto_dir)`; handshake in constructor (throws `WORKER_PROTOCOL` on mismatch). |
| Identity | `WorkerHello.worker_id` → `ExecutionRecord.worker_id`. |
| Workspace | deterministic `temp/<job_id>/<task_id>/{images/, features/, matches/, database.db, sparse/, logs/}` (worker_handle.h). COLMAP's `database.db` is a **COLMAP-private SQLite file inside the isolated task workspace** — it is NOT the MetadataDb and never touches it (ADR-038). |
| Isolation | Worker NEVER touches metadata DB, scene, or project rows (ADR-038; `check_worker_boundary.py`). Inputs arrive as CAS hashes (image + optional calibration) + `config_json`; outputs arrive as `ArtifactInfo{content_hash, type, mime, manifest_json}`. CAS + COLMAP workspace are the only surfaces the worker touches. |
| Resource claim | Worker never claims resources; scheduler is the single allocator (process-model §5, RFC-0008 §10). |
| Cancellation | Cooperative, checkpointed at CLI-tool boundaries (adding-adapter.md §6). |
| Concurrency | 1 task at a time on the worker for C1 (scheduler `max_concurrency = 1`, scheduler.h comment). |
| Crash | EOF / heartbeat timeout → `WORKER_CRASHED` → retry per `RetryPolicy` (transient) or fail with report (deterministic). |

## 5. Input Artifact Mapping

| Canonical input | Source | COLMAP consumption |
|---|---|---|
| ImageArtifact set | `SceneQuery::ArtifactHash` for session images (RFC-0008 §7). | `feature_extractor --image_path <workspace>/images`; images materialized into the per-task workspace from CAS by hash. |
| CalibrationArtifact (optional) | Content hash in `TaskRequest.input_refs` (RFC-0009 §6). Session layer resolves `SceneQuery::ResolveCalibrationAt`, writes the CalibrationArtifact to the CAS, passes its hash. | Worker materializes the payload from CAS into its workspace; `colmap_converter` maps canonical `intrinsic_model` → COLMAP camera model (RFC-0009 §5). **Absent → COLMAP self-calibrates from EXIF; absence is not a failure.** |
| GNSS/IMU/LiDAR priors | Optional; **deferred** in C1 (RFC-0008 §7 lists them as accepted priors; the first slice does not require them). | — |
| Effective configuration | `config_json` — **algorithm settings ONLY** (SIFT/matcher/mapper params, threads, seed; RFC-0008 §9). `Sha256Hex(config_json)` = ADR-020 cache key, independent of calibration. | Detector/descriptor settings, matcher thresholds, mapper options. |

Invariant (RFC-0009 §6): `input_refs` = spatial data (image hashes, optional CalibrationArtifact hash); `config_json` = algorithm configuration. **`config_json` never contains `fx`/`fy`/`cx`/`cy` or any spatial measurement** — a calibration value in the configuration surface is a contract violation rejected by validation. The engine hands the worker **content hashes + effective configuration, never SQL or scene rows** (ADR-038).

## 6. SparseModel Output Mapping

COLMAP sparse models (`cameras.bin`, `images.bin`, `points3D.bin`) → canonical SparseModel artifact (RFC-0008 §6). The mapping is defined here; **the `sparse-model.schema.json` contract itself is deferred to P2.5**.

| COLMAP element | Canonical mapping |
|---|---|---|
| `cameras.bin` (camera model id + `params`) | `calibration.schema.json` taxonomy via the adapter mapping (RFC-0009 §5): `SIMPLE_PINHOLE`/`PINHOLE` → `pinhole`, `OPENCV` → `opencv`, `FOV` → `fov`, `OPENCV_FISHEYE` → `opencv_fisheye`, `THIN_PRISM_FISHEYE` → `custom`. Intrinsics `{fx, fy, cx, cy}` in pixels. |
| `images.bin` (qvec/tvec per image) | **Pose data written into the SparseModel artifact payload** (rotation quaternion `qvec` + translation `tvec` converted per COLMAP world-from-camera convention). `WorldFromSensor` materialization into the Observation Graph is **Session/Application-layer only** (C1.1; ADR-024) — the worker never writes the Observation Graph. |
| `points3D.bin` (XYZ, track, color, error) | 3D points with per-point provenance + uncertainty channels (ADR-025) where available. |
| Track/point descriptors | Attach to observations; per-frame descriptors live in the FeatureArtifact (SIFT), not duplicated. |
| BA report (reprojection errors, view counts) | Feeds `QualityReport` (ADR-030, RFC-0005). |

Pose ownership (approved C1 split): worker ⇢ pose data inside SparseModel artifact; Session ⇢ `WorldFromSensor` materialization into Observation Graph (two-tier pattern `WriteFeatureArtifactPayload` vs `WriteFeatureArtifact`, feature_extraction.h).

Artifact metadata:
- `type: "sparse_model"` (PPS-0001 §5.3 activation).
- `coordinate_frame` per the canonical model (world); `unit: "meter"`.
- `schema_version` declared with P2.5's `sparse-model.schema.json`.

## 7. Provenance

Every canonical artifact produced by the COLMAP worker carries (RFC-0008 §8; `ArtifactManifest`, core/artifacts/artifact_manifest.h):

- `producer`: `{id: "colmap", version: <adapter version>, git_commit: <adapter commit>}`.
- `input_artifact_hashes`: the CAS hashes of the image artifacts consumed **plus the CalibrationArtifact hash when present** (RFC-0009 §6 — calibration enters the input set, independent of `configuration_hash`).
- `configuration_hash`: `Sha256Hex(config_json)` (ADR-020 cache-key contract, closed by M1; RFC-0008 §9) — **excludes calibration content**; calibration is an input, not configuration (RFC-0009 §2/§6).
- `creation_timestamp`, `coordinate_frame`, `unit`, `file_size`, `mime_type`, `validation_status`.

Provenance is never discarded (principle 12). The identity chain is already computed by `PipelineCompiler` (pipeline hash → task hash → artifact hash, AC-8); C1 only ensures the COLMAP worker populates the manifest fields the same way the mock does.

## 8. ADR-020 Cache / Resume

- **Cache key:** task inputs = sorted CAS `input_artifact_hashes` (image hashes + optional CalibrationArtifact hash) + `configuration_hash` (=`Sha256Hex(config_json)`, **algorithm settings only**) + producer version + engine git commit. Equal config + equal input ⇒ cache replay (ADR-020, AC-8). COLMAP stages are `deterministic: true`, `cache: kCacheable`.
- **Behavior:** identical re-run hits `TaskCache`, skips the worker, threads prior output refs to dependents (scheduler `ThreadOutputsToDependents`). This works for COLMAP with **no scheduler change** — the worker only ever produces content-addressed immutable artifacts.
- **Resume (C1 guarantee = scheduler replay/retry only):** `Scheduler::Resume(job_id)` reloads persisted state and drives earliest incomplete tasks (ADR-020, scheduler.h). The C1 resume contract is: same inputs + same `config_json` ⇒ same CAS result, replayed deterministically. **Native COLMAP partial-model continuation is NOT claimed in C1** — it is deferred pending verification of COLMAP checkpoint compatibility.
- **Determinism note:** COLMAP feature extraction/mapping must pin any nondeterminism (thread count, seed) via `config_json` so that equal config yields equal outputs; validated by the cache round-trip test.

## 9. ProcessRunner / ADR-028

The codebase's real worker backend is `ProcessExecutor` (engine/workers/process_executor.h); there is no separate "ProcessRunner" type today. This section aligns the plan with ADR-028 and the ratified execution surface (RFC-0008 §10):

- **Execution surface:** `Engine → Scheduler → ProcessExecutor → COLMAP process worker → Canonical Artifact → CAS → Scene`.
- **ADR-028 applies at the workflow level, not inside C1's scope.** Workflow stage semantics, checkpoint UI, and replay orchestration are deferred post-M0 (ADR-028 "Deferred to" clause). C1's responsibility is narrower: make every COLMAP stage *durable* (persisted as immutable artifacts + scheduler state) and *replayable* (ADR-020 cache), so the later workflow layer can layer undo/comparison on top without rework.
- **Durability:** COLMAP task results persist as CAS artifacts + `ExecutionRecord`s (scheduler state store) — a session survives restarts and machine hand-off. This is the C1 "ProcessRunner" contract: the process is supervised (spawn/handshake/heartbeat/cancel/shutdown) by `ProcessExecutor`, and the *outcome* is durable through the scheduler.
- **Checkpointing:** COLMAP mapper output is preserved in the task workspace so diagnostics survive a crash, but C1 resume = scheduler replay from scratch (same inputs + same `config_json` ⇒ same CAS result). Reusing partial COLMAP output across worker restarts (native continuation) is **deferred** pending checkpoint-compatibility verification (§8).
- **No new long-running "workflow runner" process is introduced in C1.** A workflow/runner abstraction, if ever needed, lands with the ADR-028 milestone and composes the same scheduler + `ProcessExecutor` primitives.

## 10. Failure Semantics

Per `reconstruction-pipeline.md` §5 and RFC-0008 §11:

| Case | Behavior |
|---|---|
| Degenerate SfM (no overlap, <2 views, insufficient matches) | `failed` task with a diagnostic `QualityReport`; partial COLMAP model preserved as a diagnostic artifact; retry re-runs from scratch (native subset-resume deferred, §8). |
| Transient worker failure (crash, heartbeat timeout, OOM) | `recoverable: true`; retry with bounded exponential backoff (`RetryPolicy`: 2 attempts, 1s base, ×2, 60s cap). |
| Deterministic failure (bad config, missing image hash, format error) | `recoverable: false`; fail with reproducible report (`TaskFailed` + `ErrorInfo` stable code). No retry. |
| Cancellation | Cooperative; stop at CLI-tool boundary; persisted as first-class `cancelled` state; never re-run (ADR-020). |
| Dependency failure | Default `FailurePolicy::kSkipped` for non-strict paths; strict stages may use `kFailed`. |
| License/environment gate | `ValidateEnvironment()` (doctor) fails → adapter not selectable; a planned/non-permissive backend never links (ADR-031, adding-adapter.md §4). |

Errors cross IPC as typed payloads (`AdapterError`/`WorkerError`) with stable codes, context, `recoverable`, and suggested action (adding-adapter.md §6, ADR-014). No exceptions across the boundary.

## 11. Tests

New test binaries under `tests/unit/` (registered in `tests/unit/CMakeLists.txt`):

| Test | File | Proves |
|---|---|---|
| Adapter descriptor + capability negotiation | `test_colmap_adapter.cpp` | Descriptor declares exactly `feature_extraction`, `sparse_reconstruction`, `bundle_adjustment`; capability negotiation rejects it otherwise (adding-adapter.md Step 8). License gate + `ValidateEnvironment` behavior. |
| Worker protocol round-trip (spawn/handshake/artifact emission) | `test_colmap_worker.cpp` | `ProcessExecutor` spawns the COLMAP worker, `WorkerHello` negotiates capabilities, `TaskRequest` → `TaskAccepted` → `TaskProgress` → `TaskArtifactProduced` → `TaskCompleted`. |
| SIFT encoding round-trip | `test_sift_encoding.cpp` | Packed `feature` payload encodes/decodes to identical keypoints/descriptors; M0 JSON-array consumers unaffected (`schema_version: 1` guard intact, `schema_version: 2` = SIFT). |
| Cache/resume over the process worker | `test_colmap_cache_resume.cpp` | Equal config + equal inputs ⇒ ADR-020 replay (no worker re-run); interrupted task re-runs from scratch on resume (scheduler retry/replay contract — native COLMAP continuation explicitly out of scope). |
| Calibration input contract | `test_colmap_calibration.cpp` | Optional CalibrationArtifact hash in `input_refs` → worker maps canonical `intrinsic_model` → COLMAP camera model; absent → self-calibration path; calibration hash in `input_artifact_hashes`, excluded from `configuration_hash`. |
| Config-surface rejection | `test_config_rejection.cpp` | Calibration value (`fx`/`fy`/`cx`/`cy`/distortion) in `config_json` rejected by validation (RFC-0009 §6 contract). |
| Failure semantics (degenerate SfM) | `test_colmap_failure.cpp` | No-overlap set → `failed` with diagnostic report; transient crash → retry → success; deterministic error → no retry. |
| Cancellation | `test_colmap_cancel.cpp` | Cooperative cancel at CLI-tool boundary → `cancelled` terminal state; not re-run. |
| Mock↔real parity (replaceability) | `test_colmap_parity.cpp` | Same stage/recipe against mock and COLMAP (when installed) → identical scene/artifact outcomes; gold fixtures generated once from a verified backend (adding-adapter.md Step 9). |
| Worker boundary | `check_worker_boundary.py` (gate) | COLMAP worker never includes/touches DB, scene, or project code (ADR-038). |

Real-backend tests are marked and skipped when COLMAP is absent (adding-adapter.md "When is it done"); the parity suite runs fully against the mock on every commit.

## 12. Architecture Gates

Every C1 commit must pass all 7 gate scripts (scripts/): `check_dependencies.py`, `check_schemas.py`, `check_rfc.py`, `check_constitution.py`, `check_domain_types.py`, `check_worker_boundary.py`, `check_arch_debt.py` — plus `check_constitution --rfc RFC-0008` (RFC-0008 §19) for COLMAP increments and `check_constitution --rfc RFC-0009` (RFC-0009 §9) for the CalibrationArtifact prerequisite increment. Commit bodies cite the governing RFC.

Architecture invariants enforced (RFC-0008 §5, §17; RFC-0009 §8; principles 9, 15, 16):
- No COLMAP-native types above the adapter boundary; COLMAP camera-model mapping is adapter-side (`colmap_converter`), never in Core.
- No Core/Scene/DB access from the worker; COLMAP `database.db` is workspace-private, never the MetadataDb.
- No backend includes outside `adapters/**` (`check_arch_debt`).
- Capability selection by capability, never by vendor (ADR-034).
- Additive-only protocol/schema changes; `feature.schema.json` SIFT vocabulary lands as `schema_version: 2` (M0 `schema_version: 1` consumers untouched); `calibration.schema.json` gains `fov` additively.
- `input_refs` = spatial data (image + optional CalibrationArtifact hashes); `config_json` = algorithm settings only (RFC-0009 §6).
- Worker writes pose data inside the SparseModel artifact; `WorldFromSensor` materialization is Session-layer (C1.1).

## 13. Scope / Non-Goals

**In scope (C1, RFC-0008 increment):** real-adapter activation (`adapters/interfaces/**`), COLMAP process worker (`adapters/colmap/**`), SIFT feature artifact encoding (`schema_version: 2`), SparseModel artifact identity + COLMAP format mapping (schema deferred), execution through `ProcessExecutor`, provenance + ADR-020 cache/resume (scheduler replay only), failure semantics, tests (mock↔real parity), registry status flip to `integrated`, doctor step.

**Prerequisite increment (RFC-0009, lands before C1 COLMAP code):** PPS-0001 §5.3/§5.8 amendment; `calibration.schema.json` additive `fov` extension; `CalibrationArtifact` producer/writer in `core/artifacts/**`; session-layer calibration→artifact materialization. C1 consumes the artifact as an optional input; C1 does not implement calibration solving.

**Deferred / explicitly out of scope (RFC-0008 §18; RFC-0009 §2):** `sparse-model.schema.json` (P2.5), dense MVS, mesh, texturing, ICP, SLAM fusion, LiDAR fusion, NeRF, Gaussian Splatting, AI backends (VGGT/LingBot reserved), Adaptive Engine production ranking, `PluginManager` dynamic loading, workflow-stage UI/replay orchestration (ADR-028 deferred clause), GPU-required path (CPU-first), all secondary backends (Ceres internal-only, GTSAM/Open3D/KISS-ICP future milestones), calibration solving (sensor-model.md §5 / ADR-031), native COLMAP partial-model continuation.

**Exit criteria for C1 (RFC-0008 §Impact/Acceptance):** a session's images produce a canonical SparseModel artifact and `WorldFromSensor` poses through the COLMAP process worker, with complete provenance and configuration hashing, cacheable under ADR-020, no Core/scene/DB access from the worker (ADR-038), and `check_constitution --rfc RFC-0008` passes.

## References

- RFC-0008 (ratified `8794bc6`) — Production Reconstruction Backend Architecture
- RFC-0009 (ratified `876d1c4`) — Calibration Artifact & Canonical Camera Calibration Model
- `docs/development/adding-adapter.md` — seam checklist steps 1–9
- `docs/development/processing-reconstruction-matrix.md` (M2, `2b978cf`) — decision + license verification
- `engine/workers/process_executor.h`, `worker_handle.h`, `in_process_executor.h`, `mock_pipeline_runner.h`
- `engine/scheduler/scheduler.h`, `engine/pipeline/pipeline_*.h`, `engine/engine.h`
- `core/artifacts/artifact_manifest.h`, `schemas/protobuf/worker.proto`
- `schemas/json/feature.schema.json`, `calibration.schema.json`, `worker-capabilities.schema.json`
- ADR-003, ADR-010, ADR-011, ADR-012, ADR-014, ADR-016, ADR-020, ADR-021, ADR-024, ADR-026, ADR-028, ADR-030, ADR-031, ADR-033, ADR-034, ADR-035, ADR-038
- `docs/specifications/reconstruction-pipeline.md`, `docs/specifications/adaptive-engine.md`
- `docs/specifications/sensor-model.md` §3.1 (Calibration record, contributing artifact hashes)
- `THIRD_PARTY.yml`, `MODEL_LICENSES.yml` (commit `2b978cf`)
