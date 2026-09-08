# P3 — Production E2E Readiness

**Status:** READINESS — audit only. **Zero code changes** were made to the tree (`core/**`, `engine/**`, `adapters/**`, `schemas/**`, `cli/**`, `tests/**`, `CMake/**`, `conanfile.txt`, `THIRD_PARTY.yml`). The single writable deliverable is this document. It maps, file-by-file and line-by-line, how the current pipeline stages and tests **bypass** the production execution path (created by the full chain `Engine → Scheduler → ProcessExecutor/worker → ProcessingAdapter → CAS/materialized workspace`), and it defines the **one golden production E2E** (the full photogrammetric correction DAG) plus its **negative E2Es**.
**Date:** 2026-09-07
**Scope tolerance:** no new algorithms, no dense / OpenMVS / GTSAM-math / COLMAP-math changes, no AI, no SLAM, no multi-session, no broad refactoring. Everything below is either *already implemented* (and shown to bypass the production path) or a *narrow, file-scoped wiring change* to make a stage reachable through the engine's own production executor.

---

## 0. The production execution path (the standard every pipeline must reach)

The platform's **only** production execution path is:

```
CLI / host
   └─ Engine::RunPipeline / Engine::RunGraph            engine/engine.h:56-64, engine/engine.cpp:77-136
        └─ PipelineCompiler::Compile (Graph builder)    engine/pipeline/pipeline_compiler.cpp:65-166
        └─ Scheduler::Run                               engine/scheduler/scheduler.h:34-40, engine/scheduler/scheduler.cpp:41-43
             └─ WorkerExecutor (injected)               engine/engine.h:44-45
                  ├─ ProcessExecutor (real worker)      engine/workers/process_executor.h:36-106
                  │     └─ child worker process          adapters/colmap/colmap_worker_main.cpp:128-193
                  │           └─ ProcessingAdapter::Execute  adapters/interfaces/processing_adapter.h:55-81
                  └─ InProcessExecutor (test-only)      engine/workers/in_process_executor.h:4-6 (ADR-011, explicitly NOT production)
```

Along this path the artifacts and DB rows flow through the **fail-closed CAS boundary** owned by the executor (`process_executor.h:9-19, 72-105`): inputs materialized into `workspace/inputs/<hash>`, produced payloads SHA-256 verified and `ArtifactStore::Put`-ingested before `kArtifactProduced`, anything failing closed to `kFailed`. The `PipelineCompiler` enforces capability binding before any task is created (`pipeline_compiler.cpp:89-98`).

Every stage below is categorized by how far it is from this path.

**Gap legend** (used in all tables):
- **BY-PASS-EXEC** — the stage runs OUTSIDE the engine executor entirely (a direct free-function call with injected seams/db), never through `Engine → Scheduler → executor`.
- **BY-PASS-ADAPTER** — the test replaces the real `ProcessingAdapter` with an in-process seam/deterministic stub.
- **BY-PASS-CAS** — inputs/outputs are synthetic/inline objects or temp-dir files rather than CAS artifacts with `ArtifactManifest` provenance.
- **BY-PASS-WORKER** — the executable that would run inside a worker subprocess is faked by `colmap_probe_shim_main.cpp`.

---

## 1. Test-only path inventory (per item)

Each row: the current (test/entry) path, the production path that must replace it, the gap, and the narrow fix required. All `file:line` evidence is against the current tree.

| # | Item (file:line) | Current path (bypass) | Production path | Gap | Required fix |
|---|---|---|---|---|---|
| 1.1 | `tests/unit/test_bundle_adjustment_optimize_pipeline.cpp:105-140` — `StubOptimizer` seam double (`rms_after_`/`reuse_v3_id_`) | Deterministic in-process stub `ReconstructionOptimizer` injected by the test; **no subprocess, no adapter, no CAS** | Real `ColmapBundleAdjustmentAdapter` (`adapters/colmap/colmap_bundle_adjustment_adapter.{h,cpp}`) implementing `core::geometry::ReconstructionOptimizer` | BY-PASS-ADAPTER + BY-PASS-CAS (v3 built inline `:62-101`, seeded straight into a temp `MetadataDb` `:164-174`) | Keep the stub for the *orchestration* unit tests (gate/ordering), but the *production E2E* must run the real adapter over a CAS-backed v3 (see §6). |
| 1.2 | `tests/unit/test_bundle_adjustment_optimize_pipeline.cpp:159` — `BundleAdjustmentOptimizePipeline(in)` free-function call | Stage invoked directly with `optimizer*`/`db*`/`source_v3*` injected in-process (header `engine/pipeline/bundle_adjustment_optimize_pipeline.h`) | `Engine::RunPipeline("sparse_correction", …)` → scheduler → executor → (worker) → stage that internally calls `BundleAdjustmentOptimizePipeline` with a real seam + the project DB | BY-PASS-EXEC (stage has **no `Register*`**, no `PipelineDefinition`, unreachable via `Engine::RunPipeline`) | Wrap the orchestration stage as a registered pipeline (`RegisterBundleAdjustment`) OR add a production `bundle_adjust` executor stage; see §2. |
| 1.3 | `tests/unit/test_bundle_adjustment_optimize_pipeline.cpp:62-101` `MakeV3`; `:164-174` `SeedSucceeded` | Synthetic `Reconstruction` (one pinhole cam + one image) constructed inline and written directly to a temp DB row | v3 read from the project DB (`QueryLatestReconstructionByScene`, `core/storage/metadata_db.cpp:1863-1896`) produced by a real upstream sparse-reconstruction run | BY-PASS-CAS (no CAS artifact, no provenance `input_artifact_hashes` chain) | Production E2E seeds v3 through the previous production stages; see §6. |
| 1.4 | `tests/unit/test_loop_closure_optimize_pipeline.cpp:144-198` — `LoopClosureOptimizePipeline(in)` free-function call + real `GtsamTrajectoryOptimizer` injection (`:148`) | Direct stage call; **real GTSAM adapter** injected in-process by the test; temp `MetadataDb` (`:110`); synthetic drifted square + closure `:70-101` | `LoopClosureOptimizePipeline` (or the chained correction stage) reachable through `Engine::RunPipeline` with a `TrajectoryOptimizer` seam bound to a worker that owns GTSAM | BY-PASS-EXEC + BY-PASS-CAS (no `Register*`; synthetic nodes/trajectory; GTSAM runs in-process not in a worker) | Register as a pipeline stage; production E2E must obtain trajectory/closure from CAS+DB produced by earlier stages; see §2/§6. |
| 1.5 | `tests/unit/test_loop_closure_detection.cpp` — `DetectLoopClosureCandidates(store, db, …)` free-function call (`engine/pipeline/loop_closure_detection.h:62-68`) | Direct call; `RegisterLoopClosureDetection` (**does** exist `loop_closure_detection.cpp:120-132`) but **not referenced by the CLI** and, per `P3-milestone-status.md:127-138` (7a.1), the `loop_closure` capability is **not bound** to the executor dispatch | `Engine::RunPipeline("loop_closure_detection", {frame/feature CAS refs}, …)` | BY-PASS-EXEC (registration exists but nothing wires it through the executor; the executor dispatches one `task_type` per stage over one input ref — multi-input FeatureArtifact consumption not wired) | 7a.1 executor-capability integration (deferred debt, `P3-milestone-status.md:123-139`): add multi-input task + capability-profile wiring. |
| 1.6 | `tests/unit/test_loop_closure_verification.cpp` — `VerifyLoopClosureGeometry(store, db, …)` free-function call (`engine/pipeline/loop_closure_verification.h:63-70`) | Direct call with in-process `L2NearestMatcher` + `FundamentalGeometricVerifier` + in-repo `MockFeatureDescriptors` | `Engine::RunPipeline` stage that consumes a `loop_closure_candidate` CAS artifact + source/target FeatureArtifacts | BY-PASS-EXEC + **no `Register*`** for verification at all (grep: only `RegisterFeatureExtraction`, `RegisterLoopClosureDetection`, `RegisterMockPhotogrammetry` in `engine/pipeline`) | Register `loop_closure_verification` as a pipeline; production E2E resolves candidate + features from CAS; see §6. |
| 1.7 | `tests/unit/test_loop_closure_optimize_pipeline.cpp:109-113` — temp `MetadataDb::Create(root_/ "project.db")`; `:114` `FindOrCreateScene` | Full trajectory/closure/pose-graph/optimization-data persisted to a throwaway temp DB | Rows persisted to the project DB under the real scene by upstream stages | BY-PASS-CAS (temp DB, no CAS backing for the graph document — `loop_closure_optimize_pipeline.cpp:84,100` put the graph/result *id* in `document_json`, the full document lives in CAS) | Production E2E uses the project DB; the graph/result CAS documents must actually be produced (see 3.). |
| 1.8 | `tests/unit/test_colmap_e2e.cpp:34-39` — `SPATIAL_COLMAP_WORKER_EXECUTABLE` / `SPATIAL_COLMAP_PROBE_SHIM_EXECUTABLE`; `:91-97` `MakeExecutor` uses both | Real `ProcessExecutor` + real `colmap_worker`, but the **backend executable is the probe shim** (`colmap_probe_shim_main.cpp`) — the only successful `Engine::RunPipeline` with the COLMAP worker, and only for a **single-stage `sparse_reconstruction`** pipeline (`:68-78`) | Multiple chained sparse-correcting stages over the COLMAP worker | BY-PASS-WORKER (probe shim fakes the sparse-reconstruction subcommand; no real COLMAP) — but this is the **closest-to-production** existing path and the model for §6 | The golden E2E (§6) reuses this harness shape but adds the correction stages and (where a real backend is required) requires a real subcommand path. |
| 1.9 | `tests/unit/test_colmap_bundle_adjustment_adapter.cpp` — drives `ColmapBundleAdjustmentAdapter` against the **probe shim** (`shim_fail` marker → `TaskFailed`, `bundle_adjuster` rebuilds points3D with x+0.5) | Real `ColmapBundleAdjustmentAdapter` (real seam impl) against a **fake `bundle_adjuster` subprocess**; CAS-free temp workspace | `BundleAdjustmentOptimizePipeline` with the real adapter over a CAS v3, enforced D5 gate, DB supersede (the P14 chain `bundle_adjustment_optimize_pipeline.cpp:131-148`) | BY-PASS-ADAPTER-chain (adapter proven standalone; the **engine orchestration + gate + DB supersede** around it is never exercised together with a real adapter) | Keep adapter unit test; the golden E2E must invoke the real adapter *through* `BundleAdjustmentOptimizePipeline` with real gate + DB. |
| 1.10 | `tests/unit/test_engine_e2e.cpp:65-78` — `InjectedWorkerExecutorRunsTasks` | `Engine::RunGraph` with an `InProcessExecutor` + `test::BigWorker()` running a one-task graph (`test::MakeTask`) | Real worker dispatch over CAS | BY-PASS-EXEC (InProcessExecutor explicitly not production, `in_process_executor.h:4-6`) | This test is fine as a local unit; it is NOT the production E2E. |
| 1.11 | `tests/unit/test_mock_pipeline_e2e.cpp` — `Engine::RunPipeline` with `MakeMockPipelineRunner` | Mock runner (default, `engine.h:50-54`) produces a deterministic payload, second identical run served from ADR-020 task cache | Real worker + real adapter | BY-PASS-ADAPTER (mock runner is the demo default, `engine/workers/mock_pipeline_runner.cpp`) | Demonstrates the cache/replay contract only; not production. |
| 1.12 | `cli/main.cpp:475-476` — `RegisterMockPhotogrammetry` + `RegisterFeatureExtraction` | **Only** these two pipelines are registered into `engine.registry()` — the demo mock and feature extraction | A registered production sparse-correction pipeline | Only 2 of the needed stages are even *registered* as executable pipelines | Register the correction pipeline(s); see §2/§6. |
| 1.13 | `core/loop_closure/loop_closure_candidate_gen.*`, `adapters/visual_matching/*`, `adapters/visual_geometry/fundamental_verifier.*` | In-repo deterministic adapters used in-process (7a/7b) | These are the *reference* adapters; production may swap real descriptor/verifier adapters behind the same seams | None for the E2E definition (they are the deterministic reference) | Keep as the deterministic reference in §6 negatives; do not fabricate. |

**Headline of §1:** every stage that performs the *interesting* production work — geometric verification (1.6), pose-graph + GTSAM optimization (1.4), and bundle-adjustment with DB supersede (1.1/1.2/1.9) — is reachable **only** through direct free-function calls in tests with injected seams and temp databases, **never** through `Engine → Scheduler → ProcessExecutor/worker`. The only two pipelines wired to the executor are `mock_photogrammetry` and `feature_extraction` (1.12), plus the single-stage `sparse_reconstruction` used in `test_colmap_e2e` (1.8, against the probe shim).

---

## 2. Executor integration

### 2.1 What already binds correctly
- The `PipelineCompiler` capability-binds every stage to the worker profile **before** building any task (`pipeline_compiler.cpp:89-98`): a pipeline stage declaring capability `X` fails compilation unless the executor advertises `X`.
- The **COLMAP worker advertises** `feature_extraction`, `sparse_reconstruction`, `bundle_adjustment` (`colmap_adapter.cpp:38-42, 81-82, 96-97`), sent in `WorkerHello` (`colmap_worker_main.cpp:58-82`). So a worker executability check for `bundle_adjustment` *can* be satisfied at the capability level.
- `engine.cpp:80-106` `RunPipeline`: `registry_.Resolve` → `compiler_.Compile(def, external_inputs, config_json, executor_->profile(), executor_->implementation_label())` → manifest begin/insert → `scheduler_->Run`. A registered `PipelineDefinition` is fully dispatched by the executor.

### 2.2 What is missing (each is a narrow, file-scoped gap)
1. **No `Register*` for verification / pose-graph / GTSAM / BA orchestration stages.** The only `Register*` functions in `engine/pipeline/` are `RegisterFeatureExtraction` (`feature_extraction.cpp:147`), `RegisterLoopClosureDetection` (`loop_closure_detection.cpp:120`), and `RegisterMockPhotogrammetry` (`mock_photogrammetry.cpp:9`). `VerifyLoopClosureGeometry` (`loop_closure_verification.h:63`), `LoopClosureToPoseGraph` (`loop_closure_to_pose_graph.h:58`), `LoopClosureOptimizePipeline` (`loop_closure_optimize_pipeline.h`), and `BundleAdjustmentOptimizePipeline` (`bundle_adjustment_optimize_pipeline.h`) define **no** `PipelineDefinition`/`Register*`. Consequently `Engine::RunPipeline` cannot address any of them.
2. **The DB-facing stages are fundamentally not worker-dispatchable today.** `BundleAdjustmentOptimizePipeline` and `LoopClosureOptimizePipeline` take injected `MetadataDb*` + seam pointers in their `Input` structs and perform **DB writes** (`bundle_adjustment_optimize_pipeline.cpp:139-148` WriteTransaction; `loop_closure_optimize_pipeline.cpp:127-129, 162, 193` `Upsert*`). The worker boundary **forbids** `MetadataDb`/`sqlite3` inside `engine/workers/**` and `adapters/colmap/**` (`scripts/check_worker_boundary.py`), and the COLMAP worker runs a *CAS-free* `ExecutionContext` (`colmap_worker.h:8-14`). So the DB-supersede logic must live **host-side**, after the worker returns the optimized document/artifact — it cannot run inside the subprocess.
3. **The `bundle_adjustment` capability is advertised but has no executable stage yet.** `ColmapConfig`/`ColmapStage` enumerate only three subcommands (`kFeatureExtractor`, `kMatcher`, `kMapper`) (`colmap_config.h:30-34`); `colmap_cli.cpp` `StageSubcommand` maps only those three (`colmap_cli.cpp:29-39`). The readiness doc `P3-impl-8c-bundle-adjustment-readiness.md:56-58` confirms there is *no* `bundle_adjuster` subcommand/plan step. So even a `RegisterBundleAdjustment` pipeline whose stage capability is `bundle_adjustment` would compile against the COLMAP worker profile but could not actually run a real `bundle_adjuster`.
4. **7a.1 loop-closure executor integration is deferred debt** (`P3-milestone-status.md:123-139`): the in-process executor dispatches **one `task_type` per stage over a single input ref**, and `DemoWorkerProfile().capabilities` carries only `feature_extraction, reconstruction, validation`. Candidate generation consumes **multiple** FeatureArtifacts per run — a multi-input worker `task_type` + capability-profile wiring must be added.

### 2.3 The production executor shape required (design, not code)
For the golden E2E the platform needs **two** executor-visible roles that today are conflated:
- A **worker-dispatched** "compute" role: `feature_extraction`, `sparse_reconstruction`, `loop_closure_detection`, `loop_closure_verification`, `bundle_adjustment` run in the subprocess, materializing CAS inputs, invoking the `ProcessingAdapter::Execute` surface (`processing_adapter.h:55-81`), and producing CAS artifacts under the executor-owned boundary (`process_executor.h:9-19`).
- A **host-side orchestration** role (the DB-facing stages `LoopClosureOptimizePipeline` and `BundleAdjustmentOptimizePipeline`) that consumes those CAS artifacts + reads/writes the project DB. This role must be invoked from the host (it owns the DB and the seam injection), not from the worker.

The minimal wiring is to add **`Register*`** functions producing `PipelineDefinition`s whose stage chains terminate in a host-runnable orchestration block (or to add a single host-runner entry that chains the existing free functions with the project DB + worker-produced artifacts). No algorithm or math changes; purely chess-move wiring of already-implemented stages.

---

## 3. Artifact lineage

### 3.1 What the canonical document carries
- `core/reconstruction/reconstruction_json.h:40-137` — `ReconstructionToJson` emits: `reconstruction_id`, `scene_id`, `session_ids[]`, `coordinate_frame`, `status`, `created_at_ns`, `provenance{backend{name,version,adapter_version}, configuration_hash, input_artifact_hashes[], engine_version, engine_commit, git_commit, started/finished/duration_ns, backend_specific_json?}`, `cameras[]`, `images[]`, `points3D[]`.
- **There is NO `parent_reconstruction_id` field** in the v2 schema (`reconstruction_json.h` serializes none; `Reconstruction` has none). The *only* explicit cross-revision lineage is `provenance.input_artifact_hashes` (`reconstruction_json.h:63-65`) — a list of **artifact content hashes**, not reconstruction ids. The structural v1→v2→v3→v4 chain is carried **implicitly** by the DB revision model (append-only status chain, `metadata_db.cpp:1970-1977`) and by `QueryLatest…ORDER BY created_at_ns DESC` repointing (`metadata_db.cpp:1863-1896`), not by an explicit parent pointer in the document.
- `core/artifacts/artifact_manifest.h:23-36` — `ArtifactManifest` carries `artifact_uuid`, `content_hash`, `type`, `producer{id,version,git_commit}`, `input_artifact_hashes[]`, `configuration_hash`, `creation_timestamp`, `coordinate_frame`, `file_size`, `mime_type`. **No parent-reconstruction field** here either; lineage across artifacts is via `input_artifact_hashes` joins on `content_hash`.

### 3.2 Consequence for lineage in the production E2E
- A `v4` produced by BA after GTSAM+collation is linked to its `v3` only through:
  - the **DB revision chain** (v3 row flipped to `superseded`, v4 inserted `succeeded`, same `scene_id`) — the P14 ordering `bundle_adjustment_optimize_pipeline.cpp:131-148`; and
  - `v4.provenance.input_artifact_hashes` **appending** the v3 chain's hashes (mirroring `triangulation.h:497-506`, `reconstruction_feedback.h:144-180`).
- Per `P3-impl-8c-bundle-adjustment-readiness.md:156` (P13), a new revision's `input_artifact_hashes` = inherited chain + the immediate predecessor id, sorted ascending, no self-reference. **No migration** is needed (`:176-177`, P17): the existing columns carry the full lineage.

### 3.3 What must be proven in the E2E (assertion target)
The golden E2E must assert the v1→v2→v3→v4 chain is recoverable *without* a parent pointer: all four rows share `scene_id`, the status chain is `superseded×3 → succeeded`, and each successor's `input_artifact_hashes` ⊇ its predecessor's (the append-only provenance chain). This is a **verifiable, non-migration** claim — see §6.

---

## 4. Real CAS path

### 4.1 What the CAS path is
`ProcessExecutor` owns the CAS boundary (`process_executor.h:9-19, 72-105`):
- **Input materialization:** each `TaskRequest.input_refs` hash is looked up in `ArtifactStore`, written to `workspace/inputs/<hash>` before dispatch; failure to materialize surfaces `kFailed` (never a silent no-op).
- **Output ingest (fail-closed):** produced payload must exist, `SHA-256(payload) == content_hash`, manifest parsed, then `ArtifactStore::Put`; only then is `kArtifactProduced` reported. Any mismatch → `kFailed`.
- The COLMAP worker runs a **CAS-free** `ExecutionContext` (`colmap_worker.h:8-14`, `colmap_adapter.cpp:142-179`): the host materializes inputs, the worker stages them into `workspace/images|calibration.json`, runs the subcommand, and the host ingests the output.

### 4.2 Current test/entry points that **do** use a real CAS (good precedents to reuse)
- `tests/unit/test_colmap_e2e.cpp`: real `ProcessExecutor` + `project_->artifacts()` → CAS `reconstruction` artifact v2; manifest `input_artifact_hashes.size()==2` (`:127-144`); CAS miss fails closed in the manifest (`:155-174`); ADR-020 replay served from the task cache (`:148-153`).
- `tests/unit/test_colmap_worker.cpp`: drives the real worker handshake, materialization, `kArtifactProduced`, failure, and cooperative cancellation — all over a real CAS-backed `ProcessExecutor`.
- `tests/unit/test_colmap_bundle_adjustment_adapter.cpp`: real adapter over the probe shim, parsing native models back to a canonical Reconstruction; asserts fresh v4 UUID, intrinsics byte-identical, resolved frame_ids, finite deterministic metrics.

### 4.3 Where the path degrades to temp-only (must be closed for §6)
- `test_bundle_adjustment_optimize_pipeline.cpp:62-101,164-174` and `test_loop_closure_optimize_pipeline.cpp:70-126,164-174`: v3 / trajectory / closure are **synthetic inline objects** written to a **temp `MetadataDb`**, with **no CAS artifact** and no provenance `input_artifact_hashes`. The ADR-020 determinism and CAS provenance guarantees are therefore not exercised for these stages.
- `test_engine_e2e.cpp` uses a temp `root_/demo.spx` (`:29-37`) but only lists manifests / runs one in-process task — not a CAS-heavy path.
- The graph/optimization-result **full documents** are never actually written to CAS today: `loop_closure_optimize_pipeline.cpp:84,100` store only the *id* in `document_json` ("full graph document lives in CAS") — but **no code writes those graph/result documents to the CAS**. This is a genuine lineage gap (see §7 open risks).

---

## 5. Real DB lifecycle + failure matrix

### 5.1 The governed lifecycle (P14 / 8c)
`BundleAdjustmentOptimizePipeline` (`bundle_adjustment_optimize_pipeline.cpp`):
1. Fail-closed on missing deps (`optimizer`/`db`/`source_v3`) → `ran=false` (`:55-66`); nullopt/empty `random_seed` → typed `ValidationError` (`:67`, `:36-45`).
2. Source must be a `succeeded` v3 (`:73-81`) — guards the later `succeeded→superseded` transition.
3. Run seam → v4; **v4 identity must be a fresh UUIDv4 ≠ v3** (`:93-103`, D-CRM-07).
4. **D5 gate**: `PassesReprojectionGate(rms_after, rms_before, 0.9)` (`reprojection.h:372-378`; enforced `:112-123`). Gate fails → **no v4 insert, no supersede**, `out.failure` carries both RMS + outlier counts.
5. Persist **inside one `WriteTransaction`**: `AddReconstruction(v4,"succeeded")` then `SetReconstructionStatus(v3,"superseded")`, commit (`:139-148`) — P14 ordering, P12 no-partial-results.

The locked transition table (`metadata_db.cpp:1970-1977`): `reconstructing→{succeeded,failed,superseded}`, `succeeded→superseded`, `failed/superseded` terminal. `QueryLatestReconstructionByScene` (`:1863-1896`) repoints after the supersede.

### 5.2 Failure matrix (each row = a DB-lifecycle failure mode and its current coverage)

| Failure mode | Guard site | Current test coverage | Production concern (must be in §6 negative E2E) |
|---|---|---|---|
| Seam/adapter throws mid-BA | exception propagates as typed `ProjectError` (ADR-014); no write (P12) | implicit; `BundleAdjustmentOptimizePipelineInput` deps-missing (`test_…:278-304`) | Adapter subprocess non-zero exit / timeout → `AdapterError`, no output consumed (`colmap_cli.cpp:106-114` `DiscoverNativeModelFiles`) |
| Gate fails (rms_after not < 0.9·rms_before) | `:112-123` returns without writes | `NonImprovingSeamFailsGateWithoutWrites` (`test_…:229-262`), `NonFiniteRmsNeverPassesGate` (`:264-276`) | Assert v3 still `succeeded`, no v4 row — **already tested**, must be re-asserted at the E2E level |
| non-`succeeded` source v3 | `:73-81` throws before any write | `NonSucceededSourceRejectedBeforeWrite` (`:318-336`) | E2E: corrupt/`reconstructing` latest row fails closed, no partial state |
| seam returns v3 id | `:93-103` throws | `SeamReturningV3IdRejected` (`:338-349`) | E2E: fresh-UUID invariant |
| invalid supersede transition | `SetReconstructionStatus` transition table throws (`metadata_db.cpp:1978-1983`) | part of `test_reconstruction.cpp:288-556` | E2E: attempted supersede of a non-succeeded row rejected |
| `random_seed` nullopt/empty | `:67` throws | `NulloptSeedFailsClosedWithTypedError` (`:306-316`) | E2E determinism gate (D6 required seed) |
| worker path: missing CAS input | executor materialization fails closed (`process_executor.h:72-75`); manifest shows `failed` | `tests/unit/test_colmap_e2e.cpp:155-174` | Golden E2E: a dangling CAS ref in the correction DAG fails the manifest, no worker dispatch |
| worker path: corrupt produced payload | fail-closed CAS ingest (`process_executor.h:77-81`) | `test_colmap_worker.cpp` (shim_fail) | Golden E2E negative: tampered output from a stage → `failed`, not partial |
| loop-closure: undeclared metric basis | `LoopClosureToPoseGraph` INV-3 → no edge, no optimize, closure still persisted (`loop_closure_optimize_pipeline.cpp:132-133`) | `UndeclaredBasisNoOptButClosurePersisted` (`test_loop_closure_optimize_pipeline.cpp:252-284`) | E2E negative: metric-ineligible closure must NOT reach GTSAM |

### 5.3 Loop-closure persistence uses writes
`LoopClosureOptimizePipeline` does DB writes via `UpsertLoopClosure` / `UpsertPoseGraph` / `UpsertOptimizationResult` (`loop_closure_optimize_pipeline.cpp:127-129,162,193`) with stable v5 UUID graph/result identities (`:33-47,144-176`). These `Upsert*` calls are **not** individually wrapped in an explicit `WriteTransaction` in the stage (unlike BA's `bundle_adjustment_optimize_pipeline.cpp:139-148`). Each `INSERT OR REPLACE` is atomic per statement, but there is **no single cross-row transaction** for "closure + graph + result" as a unit; a crash between them could leave closure+graph without the result row. This is an **idempotency-only** guarantee (re-run overwrites) not an atomicity guarantee — flag as an open risk (§7).

---

## 6. Acceptance test spec — the golden production E2E + negatives

### 6.1 Guiding rule
A production E2E must enter the chain **only** through `Engine::RunPipeline` (or `Engine::RunGraph`) with a **ProcessExecutor + worker + real (or probe-shim) adapter + real project `MetadataDb` + real `ArtifactStore`**, every input/output flowing through CAS with `ArtifactManifest` provenance. Reuse the existing harness shape from `tests/unit/test_colmap_e2e.cpp` (real `ProcessExecutor`, `project_->artifacts()`, temp project root).

### 6.2 One golden positive E2E

**Pipeline id:** `p3_sparse_correction` (a `PipelineDefinition` assembled from the existing stage inputs/outputs; see §2.3 for the *host-runner* orchestration role).

**Stage chain (all inputs/outputs via CAS):**
```
image[] (CAS)
  -> (1) feature_extraction       capability "feature_extraction"   -> feature[] (CAS)
  -> (2) sparse_reconstruction    capability "sparse_reconstruction" -> reconstruction v1 (CAS)      [or v2/v3 as produced]
  -> (3) loop_closure_detection   capability "loop_closure"          -> loop_closure_candidate[] (CAS)
  -> (4) loop_closure_verification capability "loop_closure"         -> loop_closures (+ feature[] refs) (CAS)
  -> (5) [host-runner] LoopClosureOptimizePipeline                    -> PoseGraph + GTSAM OptimizationResult (DB + CAS)
  -> (6) [host-runner] BundleAdjustmentOptimizePipeline               -> v4 (DB) + reconstruction (CAS)
```
(Stages 3-4 are the `Register*` wiring already partially present / `P3-milestone-status.md` 7a.1 debt; stages 5-6 are the host orchestration roles of §2.3.)

**Reuse vs new wiring:** the stage *runners* exist (`loop_closure_detection.cpp:42-118`, `loop_closure_verification.h:63-70`, `loop_closure_optimize_pipeline.cpp:106-213`, `bundle_adjustment_optimize_pipeline.cpp:49-151`) and the *capabilities* are declared (`colmap_adapter.cpp:38-42`). **New wiring only:** (a) the two host orchestration roles (5)(6) invoked with the project DB + worker-produced CAS artifacts; (b) the multi-input worker dispatch for (3)/(4) (7a.1); (c) the `bundle_adjuster` subcommand path so (2)/(6) can actually run a real backend when present (else probe shim for CI).

**Golden positive assertions (all grounded):**
- `Engine::RunPipeline` returns a **succeeded** manifest; every stage `implementation` reflects the executor (`process`), not the mock. (pattern: `test_colmap_e2e.cpp:114-123`).
- **DB lineage:** `FindReconstructionsByScene` returns the chain; each predecessor status `superseded`, terminal `succeeded`; `QueryLatestReconstructionByScene` → the final revision. This proves the append-only v1→…→v4 chain recoverable without a parent pointer (§3.2). (DB contract: `metadata_db.cpp:1863-1930,1970-1977`.)
- **Provenance/lineage:** every successor `Reconstruction.provenance.input_artifact_hashes` ⊇ its predecessor's, sorted, no self-reference (P13, `reconstruction_json.h:63-65`; `P3-impl-8c` P13).
- **D5 gate observable:** `BundleAdjustmentOptimizePipelineResult.gate_passed == true`, `trace.rms_after_px < 0.9 * trace.rms_before_px`, `trace` mirrors `EvaluateReprojection` on the same observation set (P7/P10).
- **CAS integrity:** each stage's output artifact `Has(content_hash)` under the real store; `ArtifactManifest.input_artifact_hashes` points at real stored hashes (§4.1).
- **Determinism (D6):** a second identical run (same pinned seed) yields identical *derived* metrics (`trace`, quality report, lineage hashes) and a task-cache hit on the pure compute stages (pattern: `test_colmap_e2e.cpp:146-153`); NOT raw backend byte-identity (`P3-impl-8c` D6/P20).

### 6.3 Negative E2Es (each enters through the same `Engine::RunPipeline` producer path)

| Negative | Scenario | Assertion (grounded) |
|---|---|---|
| N1 — BA gate fail | Corrected/optimal fixture where `rms_after` is **not** < 0.9·rms_before | manifest states run succeeded-with-no-improvement; **no v4 row**, v3 still `succeeded`; `QueryLatest` → v3 (`bundle_adjustment_optimize_pipeline.cpp:112-123`; `test_…:229-262`) |
| N2 — dangling CAS ref | One stage's recorded output hash is absent from the store | manifest `failed`, stage not dispatched to worker, no partial artifacts (`process_executor.h:72-75`; `test_colmap_e2e.cpp:155-174`) |
| N3 — corrupt worker payload | A stage returns a payload whose SHA-256 ≠ recorded `content_hash` | fail-closed CAS ingest → stage `failed`, manifest `failed`, nothing persisted to CAS (`process_executor.h:77-81`) |
| N4 — metric-ineligible closure | Closure whose trajectory metric basis is undeclared (INV-3) | closure persisted for audit, **no** PoseGraph, **no** GTSAM result (`loop_closure_optimize_pipeline.cpp:132-133`; `test_loop_closure_optimize_pipeline.cpp:252-284`) |
| N5 — no pinned seed | BA stage configuration omits/empties `random_seed` | typed `ValidationError` (D6), no write (`bundle_adjustment_optimize_pipeline.cpp:36-45,67`) |
| N6 — non-succeeded latest revision | Feed stage a scene whose latest reconstruction is `reconstructing`/`failed` | stage throws before any write; DB unchanged (`:73-81`; `test_…:318-336`) |

**Reuse vs new wiring for negatives:** N1/N4/N5/N6 are already unit-tested at the stage level and **reuse** those seams/stubs (deterministic `StubOptimizer`, `L2NearestMatcher`, `FundamentalGeometricVerifier`, synthetic fixtures) — the *only* new work is driving them through the real `Engine::RunPipeline` process path instead of direct free-function calls. N2/N3 reuse the executor's fail-closed behavior proven in `test_colmap_e2e.cpp`/`test_colmap_worker.cpp`; the *only* new work is feeding the same signals into the multi-stage correction pipeline.

---

## 7. Minimum implementation list (ordered, file-by-file)

Nothing below changes algorithms, math, schemas, or protected contracts that are frozen (8a/8b/`core/geometry/*`, `adapters/interfaces/*`, `core/trajectory/*`). It is chess-move wiring + registration of already-implemented stages.

1. **`engine/pipeline/loop_closure_verification.{h,cpp}`** — add `RegisterLoopClosureVerification(PipelineRegistry&)` with a `PipelineDefinition` whose stage declares capability `loop_closure` and consumes `{loop_closure_candidate, feature}` → `{loop_closure}`. (No behavior change to `VerifyLoopClosureGeometry`.)
2. **`engine/pipeline/loop_closure_optimize_pipeline.h` + a thin host runner** — expose a host-invokable entry that (a) reads the verified closure + trajectory (DB, `QueryLatest…`), (b) calls `LoopClosureOptimizePipeline` with the project DB + an injected `TrajectoryOptimizer` seam bound to the GTSAM adapter, (c) writes the PoseGraph/OptimizationResult **CAS documents** (currently only ids go to `document_json`, `loop_closure_optimize_pipeline.cpp:84,100`). Register as `RegisterLoopClosureOptimize`.
3. **`engine/pipeline/bundle_adjustment_optimize_pipeline.h`** — add `RegisterBundleAdjustment` so the P14 DB gate/supersede stage is addressable as a host-runner stage whose `ReconstructionOptimizer` seam is bound to the real `ColmapBundleAdjustmentAdapter`. (The stage body already implements P11/P14/P12/P9; only registration + seam binding is new.)
4. **`cli/main.cpp:475-476`** — call the three new `Register*` (verification, loop optimize, BA) alongside `RegisterFeatureExtraction`; provide the production entry-point flag that runs `p3_sparse_correction` end-to-end.
5. **`engine/workers/*` + capability-profile wiring** — implement the 7a.1 executor integration deferred at `P3-milestone-status.md:123-139`: a worker `task_type` + capability-profile that lets one stage consume **multiple** FeatureArtifact input refs (needed for candidate generation and verification), and add `loop_closure`/`loop_closure_verification`/`bundle_adjustment` to the relevant worker profiles.
6. **`adapters/colmap/colmap_config.{h,cpp}`, `colmap_cli.{h,cpp}`, `colmap_bundle_adjustment_adapter.{h,cpp}`** — complete the **`bundle_adjuster` subcommand** so the already-advertised `bundle_adjustment` capability (`colmap_adapter.cpp:38-42`) has an executable stage: new `ColmapStage::kBundleAdjuster` (`colmap_config.h:30-34`), CLI subcommand + `--input_path sparse/0 --output_path sparse_ba` + `--random_seed` reuse (`colmap_cli.cpp:20-23`), native-model **writer** (canonical → `cameras.bin/images.bin/points3D.bin`, the missing piece flagged `P3-impl-8c` §1.4/§5), and model discovery under `sparse_ba/` via `DiscoverNativeModelFiles`. Intrinsics-fixed toggles pinned OFF (P5). This is exactly `P3-impl-8c` P16; the readiness is already written.
7. **`tests/unit/*`** — add a `p3_production_e2e` test that runs `Engine::RunPipeline("p3_sparse_correction", …)` with a **ProcessExecutor + worker + probe shim** (or real COLMAP when present) executing §6.2 golden + §6.3 negatives; reuse the `test_colmap_e2e.cpp` harness (`:48-101`), `closed_square_reconstruction.h` fixture, and the `StubOptimizer`/`L2NearestMatcher`/`FundamentalGeometricVerifier` seams for the negatives.
8. **`tests/CMakeLists.txt`** — register `spatial_production_e2e_tests` mirroring the gated COLMAP target pattern (`:238-261`) and the probe-shim defines (`test_colmap_e2e.cpp:34-39`).

(Change **7** is the only fully new deliverable beyond registration/wiring; all stage *logic* already exists and is proven at unit level.)

---

## 8. Open risks / blocking unknowns

- **R1 — No real `bundle_adjuster` on CI.** The probe shim (`colmap_probe_shim_main.cpp`) is the only executable the CI can rely on. The `bundle_adjuster` subcommand (item 6) is not implemented, so the *real*-backend BA E2E can only be gated (CMake `:238-261` pattern) and the golden path in default CI runs under the probe shim. This is a **pre-existing, non-blocking** reality, re-flagged.
- **R2 — CAS documents for PoseGraph/OptimizationResult are not written.** `loop_closure_optimize_pipeline.cpp:84,100` put only the *id* in `document_json`, asserting the full graph/result document "lives in CAS" — but **nothing currently writes those documents to CAS**. Fixing this is required before the lineage claim (`QueryLatest` + CAS join) is fully honest. Item 2 (step c) addresses it; currently a latent lineage gap.
- **R3 — Loop-closure DB write atomicity.** The three `Upsert*` calls in `loop_closure_optimize_pipeline.cpp:127-129,162,193` are not wrapped in one `WriteTransaction` (unlike BA's `:139-148`); a crash between them can leave closure+graph without a result row. Idempotency hides it on re-run, but recovery mid-chain is undefined. Candidate for a narrow wrap, flagged not implemented.
- **R4 — Multi-input worker dispatch still deferred (7a.1).** Candidate generation + verification need multiple FeatureArtifact input refs; the current executor dispatches one `task_type` over one input ref (`P3-milestone-status.md:130-134`). Until item 5 lands, stages (3)(4) cannot run through the executor even once registered — the golden E2E is blocked on this one integration increment.
- **R5 — No explicit parent-reconstruction pointer.** Lineage across revisions is carried only by the DB status chain + `input_artifact_hashes` (appendix §3), not a `parent_reconstruction_id`. This is by design (`P3-impl-8c` P17: zero migrations) and fully recoverable, but any consumer expecting an explicit parent id will not find one; the E2E tests the *implicit* chain (assertion 2 in §6.2).
- **R6 — `DemoWorkerProfile` capabilities.** The default in-process profile still lists only `feature_extraction, reconstruction, validation` (`P3-milestone-status.md:133-134`). A pipeline whose stages declare `loop_closure`/`bundle_adjustment` compilation-fails unless the **injected executor** (`engine.h:44-45`) advertises them — every new pipeline must be run with an executor whose profile includes the new capabilities, else the compiler rejects it at `pipeline_compiler.cpp:91-98`.

---

## 9. Summary (deliverable message)

**Top gaps:**
1. Every production-interest stage (geometric verification, pose-graph + GTSAM, BA + DB supersede) is reachable **only** via direct in-process free-function calls with injected seams + temp DBs — **none** is registered or dispatched through `Engine → Scheduler → ProcessExecutor/worker` (§1, §2).
2. Only `mock_photogrammetry`, `feature_extraction` and the single-stage `sparse_reconstruction` (against the probe shim) are wired to the executor; the `bundle_adjustment` capability is advertised but has **no executable subcommand** and no registered BA stage (§2.2).
3. DB-facing BA/loop stages are **not worker-dispatchable** (worker boundary forbids `MetadataDb`, `check_worker_boundary.py`); they must be a host-side orchestration role consuming worker-produced CAS artifacts (§2.3).
4. A lineage gap: PoseGraph/OptimizationResult **CAS documents are declared but never written** (§8 R2).

**Minimum implementation list:** the 8 ordered file-by-file changes in §7 (registration of 3 pipeline stages + CLI wiring + 7a.1 multi-input executor integration + the `bundle_adjuster` subcommand + one `p3_production_e2e` test). No algorithm/math/schema changes; `P3-impl-8c` P16 already provides the adapter design.

**Blocking unknowns:** (a) the 7a.1 multi-input worker dispatch (R4) — blocks the two visual stages through the executor even after registration; (b) the `bundle_adjuster` subcommand (R1) — blocks a *real*-backend BA E2E on default CI (probe shim is the CI stand-in); (c) the never-written CAS documents for graph/optimization-result (R2) — blocks fully honest lineage. None of these requires new algorithms or protected-contract changes; each is a contained wiring increment already scoped elsewhere.
