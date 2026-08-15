# C1-S4 Verification Report — COLMAP Worker + Converter

- **Status:** complete. Commit `8664a56` (HEAD), parent `db224c6` (C1-S3).
- **Date:** 2026-08-15
- **Governing docs:** design/decision check `docs/development/colmap-p0-c1-s4-design.md` (approved as-is, D1–D6); plan `docs/development/colmap-p0-c1-implementation-plan.md` (Rev 3); RFC-0008 (ratified `8794bc6`), RFC-0009 (ratified `876d1c4`).

## 1. Scope delivered (approved decisions D1–D6)

| Decision | Delivered |
|---|---|
| D1 `WorkerExecutor::implementation_label()` | `engine/workers/worker_handle.h` + `"inprocess"`/`"process"` overrides; `engine.cpp:83` now uses it. Generic, backend-agnostic. |
| D2 images-only E2E | `sparse_reconstruction` pipeline E2E runs images only; worker defaults every input kind to `image`; calibration-through-worker deferred. |
| D3 provisional SparseModel JSON v1 | `colmap_converter` emits `schema_version: 1` document (cameras/images/points3D), no schema file, no schema validation. |
| D4 single-stage `sparse_reconstruction` | E2E pipeline = one stage, capability `sparse_reconstruction`; worker runs the full COLMAP plan `feature_extractor → matcher → mapper`. |
| D5 cancellation per demo_worker | `TaskProgress(substage="cancelled")` + empty `TaskCompleted` → engine `kCancelled`; no protocol change. |
| D6 THIRD_PARTY.yml COLMAP → integrated | `THIRD_PARTY.yml` COLMAP `planned → active` (gate vocabulary; BSD-3-Clause, launched never linked). |

## 2. Actual changes

**New files:**
- `adapters/colmap/colmap_converter.{h,cpp}` — the only reader of COLMAP's native binary formats (`cameras.bin`/`images.bin`/`points3D.bin`), little-endian, bounds-checked, fail-closed (truncated / unknown model id / missing file → `ADAPTER_PROCESS_FAILED`); RFC-0009 §5 camera-model mapping; deterministic sorted serialization.
- `adapters/colmap/colmap_worker.{h,cpp}` + `colmap_worker_main.cpp` — process worker: `WorkerHello` (protocol 1, adapter-descriptor capabilities, `colmap-cpu` resources, max_concurrency 1), stdin/stdout `[u32 LE len][protobuf]` framing, single-flight task loop, cooperative `CancelToken`, 1s heartbeats; bridges adapter events to frames; `ErrorCodeName` in `TaskFailed`.
- `tests/unit/test_colmap_converter.cpp` — 6 tests.
- `tests/unit/test_colmap_worker.cpp` — 4 tests.
- `tests/unit/test_colmap_e2e.cpp` — 2 tests.

**Modified files:**
- `adapters/colmap/colmap_adapter.cpp` — CAS-free input staging branch in `MaterializeInputs` (worker path, ADR-038: reads `workspace/inputs/<hash>`, fail-closed); canonicalizes the workspace to an absolute path before CLI argv (the scheduler hands relative `temp/<job>/<task>` workspaces — a relative `--image_path` would double-nest against the backend's cwd); `Execute` converts native model files → canonical JSON payload.
- `adapters/colmap/colmap_config.cpp` — `FromJson` unwraps the engine's stage-config envelope `{pipeline_id, pipeline_version, stage, config}`.
- `adapters/colmap/colmap_cli.{h,cpp}` — `SparseModelDir` + `DiscoverNativeModelFiles` (complete-model fail-closed) replacing the stand-in payload discovery.
- `adapters/colmap/CMakeLists.txt` — `colmap_converter.cpp` in the adapter lib; `spatial_colmap_worker` executable target (generated protobuf from the engine build tree, no `spatial_engine` link).
- `engine/workers/{worker_handle,in_process_executor,process_executor}.{h,cpp}` + `engine/engine.cpp` — generic `implementation_label()` (D1).
- `scripts/check_worker_boundary.py` — scan now covers `engine/workers/**` and `adapters/colmap/**`.
- `tests/unit/colmap_probe_shim_main.cpp` — mapper writes the real COLMAP binary layout (deterministic single-camera model); images-dir guard.
- `tests/unit/test_colmap_execution.cpp` — store==nullptr path re-asserted as worker staging (2 tests); E2E payload assertions → canonical JSON.
- `tests/unit/test_process_runner.cpp` — mapper assertions → native `.bin` files.
- `tests/unit/test_scheduler.cpp` — `FakeExecutor::implementation_label()`.
- `tests/CMakeLists.txt` — `spatial_colmap_worker_tests` target.
- `THIRD_PARTY.yml` (D6); `docs/development/adding-adapter.md` (Step-9 replaceability result); `docs/development/processing-reconstruction-matrix.md` (backfill); `docs/development/colmap-p0-c1-s4-design.md` (design doc, committed with the increment).

## 3. Tests

Full `ctest` (both configurations): **285/285 PASS** in **Debug** (68 s) and **Release** (55 s).

New tests (all green):
- `ColmapConverterTest` (6): round-trip provisional document, RFC-0009 camera-model mapping (all 11 model ids), deterministic record sorting, empty model, malformed files fail-closed, missing file fail-closed.
- `ColmapWorkerTest` (4): hello/capability handshake, end-to-end task → CAS `sparse_model` artifact (canonical JSON), `shim_fail` marker → `ADAPTER_PROCESS_FAILED` (not recoverable), `shim_hang` cancellation → `kCancelled`.
- `ColmapE2eTest` (2): `Engine(project, ProcessExecutor(colmap_worker))` + registered `sparse_reconstruction` pipeline → manifest `implementation == "process"` (D1), CAS sparse_model artifact, second run cache-hit (ADR-020); missing CAS input fails closed in the manifest.

Updated: C1-S3 `ColmapExecutionTest` worker-staging tests; `ProcessRunnerTest.HonorsWorkingDirectory` (native `.bin` model).

## 4. Gates

All 7 gate scripts **PASS** (plus `check_constitution --rfc RFC-0008`):
`check_dependencies.py` (COLMAP active, permissive), `check_schemas.py`, `check_domain_types.py`, `check_worker_boundary.py` (23 files incl. `adapters/colmap/**`), `check_arch_debt.py`, `check_constitution.py --rfc RFC-0008`, `check_rfc.py`.

## 5. Engine-isolation audit

- `git diff db224c6..8664a56 -- engine/` = **6 files, +18 / −1**, exclusively the generic `implementation_label()` addition (D1) — no backend-specific logic in the engine.
- `(?i)colmap` in `engine/**` and `core/**` = **0 hits**.

## 6. Boundary audit (ADR-035/038)

- The worker never touches the CAS, a database handle, or the scene: CAS-free `ExecutionContext` (`store == nullptr`), inputs consumed from `workspace/inputs/<hash>`, outputs reported as payload-path + manifest for the host's fail-closed ingest.
- `check_worker_boundary.py` extended to `adapters/colmap/**` and passes.

## 7. Non-goals honored (guardrail)

No `sparse-model.schema.json`, no schema validation, no calibration-through-worker, no input-kind protocol change, no scheduler/task-cache/retry/protocol changes, no dense MVS/mesh/texturing, no COLMAP-native type in Core. `worker.proto` and `worker_handle.h` (beyond the additive label) unchanged.

## 8. Stop

C1-S4 complete and committed (`8664a56`). **C1-S5 is NOT started.** The next increment requires explicit user approval.
