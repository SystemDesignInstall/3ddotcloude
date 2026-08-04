# Adding a New Adapter

- **Status:** ratified (P0)
- **References:** ADR-013 (plugin/adapter strategy), ADR-021 (mock adapters), ADR-034 (capability architecture), ADR-016 (testing), ADR-003 (dependency registry)
- **Chain:** Core → PluginManager → Plugin → Adapter → Algorithm. An adapter is the unit of capability that wraps a concrete algorithm; the engine selects adapters **by capability, never by vendor name**.

This guide is the complete, reviewable checklist for integrating a new backend (COLMAP, OpenMVS, GTSAM, KISS-ICP, VGGT, gsplat, Nerfstudio, a custom algorithm, ...). Steps are order-dependent; a PR is not complete until all nine pass.

## Step 1 — Register in THIRD_PARTY.yml

- Add the backend to the dependency registry with its license, version(s), and lifecycle status (`planned` / `integrated` / `deprecated`).
- **CI enforces this:** `dep-registry-validation` fails any resolved dependency or adapter not present in the registry.
- For M0, backends remain status `planned`; a `planned` backend cannot be linked into the kernel (ADR-031).

## Step 2 — Declare capabilities

- Choose capabilities from the ratified taxonomy (`SparseReconstruction`, `DenseStereo`, `BundleAdjustment`, `ICP`, `SurfaceReconstruction`, `Texturing`, `GaussianGeneration`, `LidarOdometry`, `LoopClosure`, `GnssIntegration` — extensible via RFC).
- The engine must be able to build a stage from your capabilities: each capability carries typed parameter and IO schemas. Do not invent a capability; extend the taxonomy only through RFC (Constitution-protected).

## Step 3 — Implement the ProcessingAdapter interface

- Implement `ProcessingAdapter` in the adapter module (`adapters/<name>/**`), behind the Plugin → Adapter → Capability interfaces.
- The adapter is the only place that may touch the backend's native data model and raw Eigen (ADR-018). It converts **to and from** the strict domain types and Scene/artifact contracts at the boundary.
- **Never bypass Core:** no direct storage, scene, artifact, or scheduler access from the adapter.

## Step 4 — Provide the descriptor (license reference)

- Publish an `AdapterDescriptor`: declared capabilities, input/output schema references, and a **license reference resolved against THIRD_PARTY.yml**.
- Licensing gates (e.g. VGGT commercial, MASt3R/DUSt3R research-only) are **enforced at load/selection time**, not build time. A commercial deployment must never silently run a research-only backend.

## Step 5 — Environment validation (doctor)

- Add environment checks to `doctor`: binary present, version match, license accepted, GPU/VRAM available if required, toolchain/libraries resolvable.
- `doctor` output must let an operator answer "is this backend runnable here?" before scheduling tasks.

## Step 6 — Process worker wrapper

- Wrap the backend as a **worker process** implementing the worker protocol (ADR-012): `WorkerHello` with protocol version, `WorkerCapabilities` negotiation, `TaskRequest` → progress/logs → `TaskArtifactProduced` → `TaskCompleted`/`TaskFailed`.
- Errors cross IPC as typed payloads (`AdapterError`/`WorkerError`) with stable codes, context, `recoverable` flag, and suggested action (ADR-014). No exceptions across the boundary.
- The worker gets a deterministic temp workspace under `temp/<job_id>/<task_id>` and streams stdout/stderr as `TaskLog`.

## Step 7 — Mock for tests

- Add a **mock adapter** (GoogleMock) alongside the interface implementing the same capabilities (ADR-021). Mocks must be faithful to the contract: capability negotiation, error propagation, input/output artifact handling, deterministic/cache behavior, and negative cases.
- Mocks declare the same capabilities as the real adapter so the full pipeline is testable with **no backend installed**.

## Step 8 — Capability negotiation test

- Prove the engine selects your adapter for the declared capabilities and **rejects it otherwise**.
- Cover the full matrix with mocks (ADR-016): capability requested vs. provided, parameter/IO schema validation at selection time, license-gate refusal, and error propagation.

## Step 9 — Replaceability test

- Prove the backend is **swappable without Core changes** (Principle 15): run the same stage/recipe against the mock and against the real adapter (when present) and assert identical scene/artifact outcomes.
- Replaceability is enforced by `check-arch-debt` (no backend includes outside adapters) and the architecture-review gate on `core/plugin/**` and `schemas/**`.
- Add golden fixtures generated once from a verified backend to keep mocks honest over time.

## When is it done

A new adapter is complete when: registered and CI-green, capabilities declared, descriptor published with a resolvable license, `doctor` passes, worker wrapper runs end-to-end against mocks, negotiation and replaceability tests pass, and the real-backend tests are marked (and skipped when the backend is absent).

## References

- `docs/specifications/plugin-api.md`, `docs/specifications/task-model.md`
- `docs/architecture/process-model.md`, `docs/architecture/error-model.md`
- ADR-003, ADR-011, ADR-012, ADR-013, ADR-016, ADR-021, ADR-034
