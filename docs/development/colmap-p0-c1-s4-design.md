# C1-S4 Design / Decision Check — COLMAP Worker + Converter

- **Status:** decision check (design verified against HEAD `db224c6`; NOT approved yet — no code started)
- **Date:** 2026-08-13
- **Governing RFCs:** RFC-0008 (ratified), RFC-0009 (ratified)
- **Plan source:** `docs/development/colmap-p0-c1-implementation-plan.md` (Rev 3)
- **Scope decision under review (user):** Option 4 — Worker bridge **+** COLMAP converter, one increment, with the user's order: architecture/design verification → Worker bridge → Converter → E2E verification. No auto-start of C1-S5.

## 1. What the design check verified (facts, against code at `db224c6`)

### 1.1 Worker plumbing that already exists (C1-S1 + M0)
- `WorkerExecutor` interface (`engine/workers/worker_handle.h`) — `Submit/Cancel/WaitForEvent/Shutdown/profile/id`; the scheduler drives any executor with **no scheduler change**.
- `ProcessExecutor` (`engine/workers/process_executor.{h,cpp}`) — spawn + `WorkerHello` handshake (protocol_version 1), **already materializes `input_refs` → `workspace/inputs/<hash>` before dispatch**, and performs **fail-closed CAS ingest** (payload SHA-256 verify → manifest parse → `ArtifactStore::Put`) before reporting `kArtifactProduced`. Cooperative cancellation via the `TaskCancelled` frame. `WORKER_CRASHED` on EOF/heartbeat.
- **Engine composition root already has the injection seam** — `Engine(Project, std::unique_ptr<WorkerExecutor>)` (`engine/engine.h:44`). C1-S4 needs **no engine.cpp change** to run a COLMAP-backed executor.
- `PipelineRegistry` is a dictionary; pipelines are registered by composition roots (tests/CLI). A COLMAP pipeline = **data** (`PipelineDefinition` bound by capability, ADR-034), no registry code change.
- `worker.proto` message set already contains everything the worker needs (`hello`, `task_request{task_id, task_type, spec_json, workspace, input_refs}`, `accepted`, `progress{substage}`, `log`, `artifact{ArtifactInfo+payload_path+manifest_json}`, `completed{outputs}`, `failed{ErrorInfo}`, `cancelled`, `heartbeat`, `shutdown`). **No protocol change required.**
- Cancellation convention (from `demo_worker.py`): the worker signals cancellation with `TaskProgress(substage="cancelled")` then an empty `TaskCompleted`; `ProcessExecutor::NextEvent` maps exactly this to `kCancelled`. The C++ worker must mirror this convention.
- Worker boundary (ADR-038): the worker never touches CAS/DB; it consumes inputs from its workspace only. `check_worker_boundary.py` enforces this.

### 1.2 Where the ColmapConverter lives (per plan §1.1, §6, RFC-0008 §6/§17)
- `adapters/colmap/colmap_converter.{h,cpp}` — **the only file allowed to read COLMAP's native binary formats** (`cameras.bin`, `images.bin`, `points3D.bin`).
- Invoked **by `ColmapAdapter::Execute`** after the CLI stages complete: native outputs → converter → canonical `sparse_model` payload → `ResultSink::ArtifactProduced`. The worker stays thin (protocol bridge only). Core never sees COLMAP types (`check_arch_debt`).

### 1.3 Canonical SparseModel format pre-P2.5
- `sparse-model.schema.json` is **deferred to P2.5** (plan §6, §13). C1-S4 must **not** invent it.
- Pre-P2.5 internal representation (proposed, forward-compatible): a documented JSON document carrying
  - camera: `intrinsic_model` from the `calibration.schema.json` enum via the RFC-0009 §5 mapping (`SIMPLE_PINHOLE`/`PINHOLE`→`pinhole`, `OPENCV`→`opencv`, `FOV`→`fov`, `OPENCV_FISHEYE`→`opencv_fisheye`, `THIN_PRISM_FISHEYE`→`custom`) + `intrinsics {fx,fy,cx,cy}`;
  - images: `image_id, camera_id, name, qvec_xyzw[4], tvec_xyz[3]` (pose data written by the worker into the artifact, plan §6; `WorldFromSensor` materialization is Session/C1.1);
  - `points3D`: `point3D_id, xyz[3], rgb[3], error, track[[image_id, point2D_idx], …]`;
  - `schema_version: 1` marked **provisional** (P2.5 schema supersedes); no schema file, no schema validation.
  - Deterministic ordering + canonical number formatting ⇒ equal inputs ⇒ equal bytes (ADR-020).

### 1.4 Input-kinds gap (the one real finding)
`TaskRequest` carries `input_refs` (hashes) but **no input kinds**. The adapter's `ExecutionContext.input_kinds` distinguishes image vs calibration. The engine materializes only raw bytes, so a worker cannot classify a calibration ref from the workspace alone.
- **Recommended decision:** C1-S4's E2E runs **images-only** (no calibration through the worker; COLMAP self-calibrates — absence is not a failure, plan §5). The worker defaults every input kind to `image` (the adapter already defaults missing kinds to `image`). Calibration-through-the-worker is a later increment, *after* the canonical SparseModel artifact exists — matching the user's stated ordering ("Сначала лучше замкнуть execution → reconstruction → canonical artifact").
- This avoids any `worker.proto` / `TaskRequest` / engine change for kinds. A generic `input_kinds` field remains a possible additive future improvement if another typed-input adapter needs it.

### 1.5 Engine changes required (all generic; each proven non-COLMAP)
1. **Worker label for execution manifests (recommended, small):** `engine.cpp:83` hardcodes `"inprocess"` into `PipelineCompiler::Compile`, which lands in `ExecutionManifest.stage.implementation`. Add a generic `virtual std::string implementation_label() const = 0;` to `WorkerExecutor` (`InProcessExecutor` → `"inprocess"`, `ProcessExecutor` → `"process"`), used by `engine.cpp`. No COLMAP identifier anywhere; additive interface method. Needed for provenance correctness (C1 exit criteria).
2. **Everything else: none.** Injection seam exists; registry is data; scheduler unchanged; protocol unchanged; `worker.proto` unchanged; `worker_handle.h` unchanged.

## 2. Proposed C1-S4 scope

### A. Worker bridge (`adapters/colmap/`, per plan §1.1)
- `colmap_worker_main.cpp` — process entrypoint: `WorkerHello` (protocol_version 1, `worker_id` UUID, capabilities = adapter descriptor, resources = `colmap-cpu` profile, `max_concurrency 1`), stdin/stdout `[u32 LE len][protobuf]` framing loop, dispatch `TaskRequest`/`TaskCancelled`/`Shutdown`, heartbeats.
- `colmap_worker.cpp` — task body (mirrors `demo_worker.py`): `TaskAccepted` → build `ExecutionContext{workspace, store=nullptr, input_refs, input_kinds={image,…}, config_json=spec_json}` → adapter `CreatePlan` + `Execute` → bridge adapter events to frames (`Progress→TaskProgress{substage}`, `Log→TaskLog`, `ArtifactProduced→TaskArtifactProduced{payload_path, manifest_json}`) → `TaskCompleted{outputs}`.
- **Adapter change (worker-path input staging):** `ColmapAdapter::MaterializeInputs` gains the pre-materialized branch — when `store==nullptr`, stage inputs from `workspace/inputs/<hash>` (local files the engine already materialized) into `images/` + `calibration.json`; missing file ⇒ fail-closed. When `store!=nullptr`, the existing CAS path (C1-S3) is unchanged. This is what keeps the worker CAS-free (ADR-038).
- Error translation: adapter `ProjectError` → `TaskFailed{ErrorInfo}` with `ErrorCodeName(e.code())` (stable string), `recoverable`, suggested action — no exceptions across the boundary (adding-adapter.md §6). Timeout maps to `ADAPTER_PROCESS_TIMEOUT` (recoverable), cancellation to `TaskProgress(substage="cancelled")` + empty `TaskCompleted` (the engine's existing `kCancelled` mapping).

### B. COLMAP converter (`adapters/colmap/`)
- `colmap_converter.{h,cpp}`: `ParseSparseModel(cameras_bin, images_bin, points3D_bin)` → document struct; `ToJson(...)` canonical serialization; RFC-0009 §5 camera-model mapping table. Binary layout documented in the header (single source of truth for the test fixture builder).
- Deterministic (sorted ids, canonical formatting). Emits the §1.3 document.

### C. E2E + tests
- The probe shim (test-only) gains a **writer** for the three `.bin` files (real COLMAP binary layout, per the documented reference in `colmap_converter.h`) so the full worker → converter path runs with no COLMAP install.
- New tests (registered in `tests/CMakeLists.txt`):
  - `test_colmap_converter.cpp` — parse→document→JSON round-trip, determinism (equal bytes), mapping table, malformed-input failure.
  - `test_colmap_worker.cpp` — spawn the COLMAP worker via `ProcessExecutor` (pattern of `test_process_executor.cpp`), assert `WorkerHello` capabilities, `TaskRequest → TaskAccepted → TaskProgress(substages) → TaskArtifactProduced → TaskCompleted`, CAS-ingested artifact, worker-boundary (no DB/scene includes).
  - E2E: `Engine(project, ProcessExecutor(colmap_worker))` + registered single-stage `sparse_reconstruction` pipeline (images-only) → manifest with a CAS `sparse_model` artifact; cache replay (second run cache-hit, ADR-020); failure (`shim_fail` marker → `TaskFailed` with stable code, no retry); cancellation.
- Gates: all 7 (`--rfc RFC-0008`), plus `git diff --stat -- engine/` limited to the single generic `implementation_label` addition (engine stays COLMAP-free).

### D. Registry / governance updates
- `THIRD_PARTY.yml`: COLMAP `planned` → `integrated` at the C1-S4 commit (plan §1.2: the commit that proves the worker runs end-to-end). COLMAP is BSD-3-Clause (permissive) — `check_dependencies.py` stays green.
- `docs/development/adding-adapter.md` Step-9 replaceability note + `processing-reconstruction-matrix.md` backfill (reference only).

## 3. Non-goals (scope-creep guard, user point 5)
- **No** `sparse-model.schema.json` (P2.5), **no** schema validation of the provisional document.
- **No** calibration input through the worker (later increment), **no** calibration solving.
- **No** `WorldFromSensor`/Observation Graph materialization (Session, C1.1), **no** Scene/DB writes from the worker.
- **No** scheduler / task-cache / retry-policy / protocol changes; **no** resume semantics beyond existing ADR-020 replay.
- **No** dense MVS / mesh / texturing; **no** new worker protocol fields.
- The converter does not introduce a COLMAP-native type into Core.

## 4. Execution order (after this check is approved)
1. `colmap_converter.{h,cpp}` + `test_colmap_converter.cpp` (pure, no worker).
2. Worker: `colmap_worker_main.cpp` / `colmap_worker.cpp` + worker-path staging branch in `ColmapAdapter::MaterializeInputs`.
3. Generic engine addition: `implementation_label()` on `WorkerExecutor` (+ `engine.cpp`).
4. Shim `.bin` writer + `test_colmap_worker.cpp` + E2E/cache/failure/cancel tests.
5. Debug + Release build, full ctest, 7 gates, engine-isolation audit, commit, verification report, **STOP (no C1-S5 auto-start).**

## 5. Open decisions for approval
- **D1:** Add generic `implementation_label()` to `WorkerExecutor` (manifest provenance correctness) — include or defer? *(recommend include)*
- **D2:** C1-S4 E2E is images-only (calibration through the worker deferred to a later increment) — confirm. *(recommend defer)*
- **D3:** Pre-P2.5 canonical SparseModel = documented JSON document, `schema_version: 1` provisional, no schema file — confirm.
- **D4:** E2E pipeline shape = single-stage `sparse_reconstruction` (capability `sparse_reconstruction`, worker runs the full COLMAP plan `feature_extractor → matcher → mapper`) — confirm.
- **D5:** Cancellation mirrors `demo_worker.py` (`TaskProgress(substage="cancelled")` + empty `TaskCompleted`) — confirm (no protocol change).
- **D6:** `THIRD_PARTY.yml` COLMAP `planned` → `integrated` at the C1-S4 commit — confirm (plan §1.2).
