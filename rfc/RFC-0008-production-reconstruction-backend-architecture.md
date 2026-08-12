# RFC-0008 — Production Reconstruction Backend Architecture

- **Status:** draft
- **Author:** Spatial Platform Architecture Board
- **Date:** 2026-08-12
- **Supersedes:** none
- **Depends on:** RFC-0002, RFC-0003, RFC-0005, RFC-0006, RFC-0007, PPS-0001, ADR-004, ADR-005, ADR-006, ADR-011, ADR-012, ADR-020, ADR-021, ADR-026, ADR-028, ADR-031, ADR-033, ADR-034, ADR-035, ADR-038, ADR-039
- **Protected surfaces touched:** Plugin API (`adapters/interfaces/**` — real-backend activation of `ProcessingAdapter`/`AdapterDescriptor`, `adapters/colmap/**`), Engine Execution (`engine/**` — the real-adapter execution path through `ProcessExecutor`/worker protocol for a COLMAP process worker), Artifact Format (`core/artifacts/**` — `sparse_model` artifact type activation; `feature` packed encoding + SIFT vocabulary). Capability API is unchanged: `feature_extraction`, `sparse_reconstruction`, `bundle_adjustment` are already ratified in the taxonomy.

## Summary

RFC-0008 ratifies the **first production reconstruction path** of the Spatial Platform: a classical, deterministic stack in which **COLMAP** is the first production reconstruction backend behind the ratified `ProcessingAdapter`/`AdapterDescriptor` seam (RFC-0007 §7), executed through the real worker surface (`Engine → Scheduler → ProcessExecutor → COLMAP process worker → Canonical Artifact → CAS → Scene`). It ratifies the **SparseModel artifact identity and the COLMAP format mapping**; the `sparse-model.schema.json` contract is explicitly deferred to P2.5 (consistent with how `feature.schema.json` landed with P2.3 in RFC-0007). It records the scope boundaries and backend dispositions decided in the M2 architecture matrix (`docs/development/processing-reconstruction-matrix.md`, commit `2b978cf`): OpenMVS excluded on license (AGPL-3.0), VGGT as a prior-only research capability outside this RFC's implementation scope, gsplat/Nerfstudio deferred, LingBot/Robbyant reserved as a future AI capability (not a production dependency, not implemented in the first milestone). **AI is explicitly out of scope and never appears as a production dependency or implementation requirement of this RFC.**

Two founding principles are ratified with this RFC:

- **P-A. COLMAP is the first production backend, not the canonical spatial model.** The Canonical Spatial Model (RFC-0002, PPS-0001) is the domain; adapters translate backend output into it, never the reverse. Replacing COLMAP must not require changes to Core, Scene, or Artifact Store (CONSTITUTION principles 15, 16).
- **P-B. Classical production path first, AI as a later capability layer.** This RFC builds the first working production pipeline on the classical deterministic stack. AI joins later through the same adapter boundary as another backend, without rebuilding the foundation.

## Motivation

P2.3 delivered the feature-extraction capability with a **deterministic mock** behind the adapter interface (RFC-0007); the exit gate (`docs/development/p2.3-completion-review.md` §10) defers every real backend until a ratified RFC. The M2 matrix resolved the decision: COLMAP-led capability stack, with licenses verified from primary sources on 2026-08-12. Three structural gaps block the first production path:

1. **No ratified execution surface for a real backend.** `ProcessExecutor` (RFC-0003 §5.7, ADR-011/012) already spawns and supervises worker processes, but every pipeline stage in the shipped platform is the in-process mock (ADR-021). There is no ratified contract for a real adapter worker: how a COLMAP process worker is spawned, which protocol it speaks, how it streams progress, and how its artifacts land in the CAS.
2. **No ratified SparseModel artifact identity or format mapping.** PPS-0001 §5.3 lists `sparse_model` as a canonical artifact type and `reconstruction-pipeline.md` §2.3 names the SparseModel artifact, but there is no ratified definition of what a canonical sparse model contains or how COLMAP's database/model formats convert to it. Without it, P2.5 cannot begin.
3. **Backend disposition is decided but not recorded in governance.** The M2 verdicts (COLMAP primary, OpenMVS excluded, VGGT prior-only, LingBot blocked) live in a development document; RFC-0008 records them in the governance layer so future milestones and agents operate from ratified facts.

## Design

### 1. Decision

**COLMAP is ratified as the first production reconstruction backend** for the capability stack `feature_extraction` + `sparse_reconstruction` + `bundle_adjustment`. It is an implementation choice behind the Capability API (ADR-034, CONSTITUTION principle 8), not a domain-model change (principles 2, 15, 16; P-A). The first production vertical slice is:

```
ImageArtifact → Feature Extraction → COLMAP → Sparse Model → Camera Poses
            → Sparse Reconstruction Artifact
```

### 2. Scope

**In scope:**

- Activation of the real adapter seam for one backend: `ProcessingAdapter`/`AdapterDescriptor` behind `adapters/interfaces/**` with a COLMAP implementation declared in `adapters/colmap/**` (status transitions from the RFC-0007 placeholder).
- The real execution surface (§10): `Engine → Scheduler → ProcessExecutor → COLMAP process worker → Canonical Artifact → CAS`.
- SparseModel artifact **identity** and the **COLMAP format mapping** (§6, §7). `sparse-model.schema.json` is **deferred to P2.5**.

**Out of scope (later RFCs):** Dense MVS, Mesh, Texturing, ICP, SLAM fusion, LiDAR fusion, NeRF, Gaussian Splatting, the Adaptive Engine's production ranking, `PluginManager` dynamic loading, and all AI backends. They are not requirements of the first backend.

### 3. Why COLMAP

- **License:** BSD-3-Clause, verified from the primary source (`colmap/README.md`, 2026-08-12) in M2.
- **Architecture fit:** already the ratified default SfM backend (ADR-004); capability-first selection is wired (ADR-034).
- **Scope:** one auditable toolchain covering the first slice — SIFT feature extraction, matching, incremental and global (GLOMAP) mapping, and Ceres-based bundle adjustment.
- **Convertibility:** COLMAP's output formats (`cameras.bin`/`images.bin`/`points3D.bin`) convert to canonical artifacts with published converter tooling; VGGT's own COLMAP-format export confirms the format is a de-facto interchange standard.
- **Maintenance:** actively maintained (2026 releases). Not selected for superiority over every engine — selected because it is permissive, complete for the first slice, and auditable.

### 4. Capability boundaries

Capabilities are the contract; COLMAP is one implementation. The engine selects adapters **by capability, never by vendor** (ADR-034, principle 8). For this RFC: a worker declaring `feature_extraction`, `sparse_reconstruction`, and `bundle_adjustment` is eligible to serve the first slice; the COLMAP adapter is the first such worker. No capability name changes; the taxonomy is untouched.

### 5. Adapter boundary

- The `ProcessingAdapter`/`AdapterDescriptor` seam introduced by RFC-0007 §7 is **activated** for real use behind `adapters/interfaces/**`.
- `adapters/colmap/**` becomes the first **process-worker** adapter, added via `docs/development/adding-adapter.md` steps 1–9 (Step 1 registry, Step 2 capability declaration, Step 3 `ProcessingAdapter`, Step 9 mock↔real parity).
- The adapter wraps the COLMAP CLI in the worker process (`feature_extractor` → `matcher` → `mapper` → `model_converter`) and converts COLMAP outputs to canonical artifacts. It never writes Core, Scene, or CAS structures directly from the backend; conversion happens in the adapter (principles 9, 15).
- **Ceres is an internal detail of the COLMAP integration** (COLMAP links Ceres for BA). It must never be exposed through Core or the engine; the platform consumes COLMAP's results, not its solver (ADR-038). This is stated to prevent the architecture being fixed as if COLMAP must use Ceres through the platform's own interfaces.

### 6. Canonical artifacts

Ratified in this RFC:

- **Identity:** `sparse_model` is an activated canonical artifact type (PPS-0001 §5.3), representing a sparse reconstruction: camera poses, intrinsics, 3D points, tracks/point descriptors, and a BA report.
- **COLMAP format mapping:** COLMAP sparse models (`cameras.bin`, `images.bin`, `points3D.bin`, with `qvec/tvec`/`params` conventions) map to the canonical sparse model — COLMAP camera models map onto the `calibration.schema.json` taxonomy (`fov`, `brown_conrady`, ...); poses convert to `WorldFromSensor` on the Observation Graph; points carry per-point provenance and uncertainty channels (ADR-025) where available. The mapping is defined in detail at implementation time; the schema version of the canonical contract is ratified with P2.5.
- **`feature` artifact extension:** COLMAP SIFT names are ratified into the feature vocabulary, and the packed/binary encoding reserved by `feature.schema.json` (header note) is approved for the COLMAP adapter with a future `schema_version`. M0 JSON-array consumers never parse it.

`sparse-model.schema.json` is **explicitly deferred to P2.5**, in line with the principle of not extending the canonical model before a real production use-case exists.

### 7. Input/output contracts

- **Canonical input:** the ImageArtifact set of a session (CAS content hashes via `SceneQuery::ArtifactHash`, RFC-0007), plus intrinsics from `calibration.schema.json` and optional GNSS/IMU/LiDAR priors. The engine remains scene-agnostic (ADR-038) and hands the worker content hashes + effective configuration, never SQL or scene rows.
- **Canonical output:** SparseModel artifact (poses, tracks, points3D), BA report, and per-frame FeatureArtifact (SIFT). Poses are written as `WorldFromSensor` with covariance on the Observation Graph; quality metadata feeds `QualityReport` (ADR-030, RFC-0005).
- **Immutability:** all outputs are CAS-addressed and immutable (principle 7, ADR-010); re-runs deduplicate (ADR-020).

### 8. Provenance

Every artifact carries its producer (adapter identity + version + git commit), `input_artifact_hashes`, and effective configuration hash (§9) in the `ArtifactManifest`. AI-derived inputs, when they later exist, enter exclusively as validated priors on observations (principle 4, ADR-006) and are never authoritative geometry. Provenance is never discarded (principle 12).

### 9. Configuration hashing

The effective stage configuration digest is the ADR-020 cache-key contract (`Sha256Hex(config_json)`, closed by M1). COLMAP adapter parameters (detector/descriptor settings, matcher thresholds, mapper options) join the `config_json` surface of the `feature_extraction`/`sparse_reconstruction`/`bundle_adjustment` stages under this RFC's change-control — the P2.3 completion-review follow-up #3 (real backend configuration surface) is hereby opened and governed. Equal config + equal input ⇒ cache replay (ADR-020, AC-8).

### 10. Execution and resource requirements

Ratified real execution surface:

```
Processing Engine → Task → Process Worker (ProcessExecutor) → COLMAP → Artifacts
```

- The COLMAP adapter runs as an **out-of-process worker** (ADR-028, ADR-011/012) spawned and supervised by `ProcessExecutor` (`engine/workers/process_executor.h`), speaking the framed protobuf worker protocol: `WorkerHello` capability negotiation, `TaskRequest` with `input_refs`/`config_json`/`workspace`, and `WorkerEvent` progress/log/`ArtifactProduced`/terminal events.
- The scheduler is the single allocator (process-model §5); the worker never claims resources itself.
- **Resume/cache:** deterministic stages replay from cache and resume from the earliest incomplete task (ADR-020); COLMAP mapper checkpoints allow stage-level resume.
- **Resources:** CPU-first (SIFT + BA on CPU); CUDA optional for feature-extraction speed. GPU is **not required** for the first slice. Runtime/resource requirements are declared per adapter in the `AdapterDescriptor`/`ResourceProfile`.

### 11. Failure semantics

Per `reconstruction-pipeline.md` §5: degenerate configurations (no overlap, insufficient images) produce a `failed` SfM task with a diagnostic report; partial models are preserved as artifacts and the stage may resume from a persisted subset (ADR-020); transient worker failures retry with backoff; deterministic failures fail with a reproducible report. Cancellation is cooperative and check-pointed.

### 12. Secondary backends (roles, not replacements)

- **Ceres** (BSD-3-Clause): nonlinear solver, **internal to adapter integrations** (inside COLMAP today). Standalone use only if a future adapter needs it; never through Core.
- **GTSAM** (BSD-2-Clause): factor-graph pose/loop-closure optimization for the SLAM milestone (ADR-005); consumes canonical poses/constraints.
- **Open3D** (MIT): ICP/registration and surface processing for later milestones.
- **KISS-ICP** (MIT): LiDAR odometry as a second reconstruction input for the SLAM milestone.

These complement COLMAP by capability; none replaces it, and none is part of the first slice.

### 13. VGGT status

VGGT is a **prior-only research capability**, not a P3 production backend and **not an implementation requirement of this RFC**. Ratified dispositions (M2, verified from `facebookresearch/vggt/LICENSE.txt` 2026-08-12): custom VGGT License v1 (2025-07-29), commercial-use friendly code with an Acceptable Use Policy (military/ITAR excluded) and amendable by Meta; the original `VGGT-1B` checkpoint is non-commercial, while the `VGGT-1B-Commercial` checkpoint provides a commercial path via application form. VGGT outputs enter only as validated priors (ADR-006), and its registration in `THIRD_PARTY.yml`/`MODEL_LICENSES.yml` reflects the verified terms. Future AI capabilities require their own RFC.

### 14. OpenMVS exclusion

OpenMVS (AGPL-3.0, verified `cdcseacave/openMVS/LICENSE` 2026-08-12) is **excluded from the first production stack and from the kernel** (ADR-031). AGPL obligations (source disclosure / commercial license) make kernel linkage non-negotiable; any future use is only via the separate-adapter process after legal review. This is a deliberate example of license as an architectural decision criterion.

### 15. LingBot / Robbyant status

**LingBot is not a production dependency of RFC-0008 and is not implemented in the first classical reconstruction milestone. Its integration remains an explicitly reserved future backend/capability, subject to source, license, model, runtime and canonical-artifact verification before implementation.** Current evidence: `github.com/LingBot/LingBot-Map` returns 404 (verified 2026-08-12), so no implementation dependency may be founded on the LingBot family (Map/Vision/Video/VA/World/VLA) today; the M2 backend decision gate is re-run when an accessible, documented upstream with a verifiable license, API, and output contract appears. The AI branch of the architecture — the right-side path `AI backend → Adapter boundary → Canonical Artifacts` (LingBot family, VGGT, and future models) — is **architecturally preserved as a reserved capability** behind the same `Capability → Adapter → Canonical Model` seam; a future RFC (e.g. RFC-0009, AI Spatial Backend Architecture) defines whether AI acts as replacement, prior, refinement, fusion, semantic layer, or mapping, once the classical COLMAP pipeline is operational. Reservation is not implementation: the reserved branch must not appear as a production dependency of any classical milestone.

### 16. License constraints

- The first backend's licenses are all permissive: COLMAP BSD-3-Clause, plus permissive dependency audits (Ceres, PoseLib, SIFT-GPU, VLFeat). COLMAP's README warns that third-party build dependencies may affect the resulting license; the configured build must be audited to exclude any GPL component.
- Registry governance is enforced by `check_dependencies.py` (ADR-003/031): `status: planned` backends never link into the kernel; non-permissive planned licenses are legal-review warnings; `MODEL_LICENSES.yml` governs model weights (no non-commercial model in the commercial build). RFC-0008's dispositions are already reflected in the registries (commit `2b978cf`).

### 17. Replacement strategy

```
                Capability
                    │
          ┌─────────┴─────────┐
          ▼                   ▼
    COLMAP Adapter       Future Adapter
          │                   │
          └─────────┬─────────┘
                    ▼
             Canonical Model
                    │
                    ▼
              Downstream
```

**COLMAP is an implementation choice, not the domain model.** Replacing it (e.g. with a future backend, or an AI adapter later) requires changes only inside the adapter layer — never Core, Scene, Artifact Store, or Scheduler (principles 15, 16, P-A). The first slice must not introduce COLMAP-native types or flows above the adapter boundary.

### 18. Non-goals

Dense MVS, mesh, texturing, ICP, SLAM fusion, LiDAR fusion, NeRF, Gaussian Splatting, AI backends, the Adaptive Engine's production ranking, `PluginManager` dynamic plugin loading, and `sparse-model.schema.json`. Each is a future milestone/RFC. The first backend is validated as a **minimal vertical slice**, so every contract can be validated independently rather than in one oversized milestone.

### 19. Implementation change-control

- **No implementation before ratification.** This draft does not authorize any change to `spatial-platform`.
- After ratification, implementation proceeds **only** in increments behind the ratified interfaces, each citing `RFC-0008` in the commit body (`check_constitution --rfc RFC-0008`).
- `core/plugin/**` and `adapters/interfaces/**` are opened by this RFC's ratification; `core/plugin/**` `PluginManager` dynamic loading remains deferred (RFC-0007 §7, ADR-034).
- P2.5 (Bundle Adjustment, `SparseModel`) is **not begun** by this RFC; it is the next capability milestone and may carry its own RFC for the `sparse-model.schema.json` contract.
- Future capability backends (dense/mesh/texture/Gaussian/SLAM/AI) each require their own RFC.

## Compatibility

- **Additive only.** No existing capability, schema, artifact type, or API changes semantics. The mock `feature_extract` runner (RFC-0007) remains the in-process implementation; the COLMAP worker is a second implementation behind the same capability (ADR-021).
- `feature.schema.json` gains SIFT vocabulary and a packed encoding via a future `schema_version`; the M0 JSON-array representation and its consumers are unaffected.
- `sparse_model` is an activation of a canonical name already reserved in PPS-0001 §5.3; no existing artifact changes meaning.
- Worker protocol changes (if any are required for the COLMAP worker) are additive to the framed protobuf surface (ADR-011/012).
- Registry dispositions already committed in M2 (`2b978cf`); no further registry edits are required by this RFC.

## Alternatives

- **OpenMVS-led first stack.** Rejected: AGPL-3.0 (verified) makes kernel linkage non-negotiable; the license is an architectural criterion (M2 §2, §16).
- **VGGT-led first stack.** Rejected: priors-only by constitution (principle 4, ADR-006), commercial path friction (application, custom license amendable by Meta), and GPU runtime — while the first slice must be a deterministic, auditable classical path (P-B).
- **In-kernel direct COLMAP linkage.** Rejected: violates principles 15/16 and P-A — the platform would become "COLMAP Platform".
- **Vendor-named pipeline types (`ColmapModel`, ...).** Rejected: canonical names only (PPS-0001 §5.8, principle 8).
- **Ratify `sparse-model.schema.json` in this RFC.** Rejected: extend the canonical model only when the real production use-case exists (P2.5), matching the RFC-0007/`feature.schema.json` pattern.
- **Oversized first milestone (full reconstruction at once).** Rejected: the minimal vertical slice keeps each contract individually validatable (M2 scope boundary; §18).

## Open Questions

- The exact structure of the canonical sparse model payload (beyond identity and COLMAP mapping) is resolved in P2.5 with `sparse-model.schema.json`.
- Whether the COLMAP worker is a C++ worker binary, a wrapped CLI, or a Python driver is an implementation detail resolved after ratification behind the adapter interface.
- The precise `schema_version` of the packed `feature` encoding is declared with the COLMAP adapter implementation.

## Impact

- **Modules (after ratification):** `adapters/interfaces/**` (real-adapter activation), `adapters/colmap/**` (COLMAP process worker), `engine/**` (real-adapter execution path through `ProcessExecutor`; worker-protocol additive extensions), `core/artifacts/**` (`sparse_model` type activation). No changes before ratification.
- **Schemas:** `feature.schema.json` (SIFT vocabulary, future packed `schema_version`); `sparse-model.schema.json` deferred to P2.5.
- **Docs:** `docs/development/processing-reconstruction-matrix.md` (source of this decision), `docs/development/adding-adapter.md` (seam executed for COLMAP), `docs/development/p2.3-completion-review.md` (§10 gate → closed), `project-context-summary.md` (P2.3/P2.4/P2.5 status), `reconstruction-pipeline.md` (SfM stage adapter linkage).
- **Tests:** after ratification — COLMAP worker contract tests (spawn/handshake/artifact emission), mock↔real parity (adding-adapter.md Step 9), SIFT encoding round-trip, cache/resume over the process worker, failure-semantics tests (degenerate SfM).
- **Acceptance:** a session's images produce a canonical SparseModel artifact and `WorldFromSensor` poses through the process worker, with complete provenance and configuration hashing, cacheable under ADR-020, with no Core/scene/DB access from the worker (ADR-038); `check_constitution --rfc RFC-0008` passes.

## References

- `docs/development/processing-reconstruction-matrix.md` (M2; the decision and license verification this RFC records)
- `docs/development/p2.3-completion-review.md` (§10 exit gate; follow-up #3 configuration surface)
- `docs/development/adding-adapter.md` (adapter seam, steps 1–9)
- RFC-0002 (§6.x canonical model), RFC-0003 (§5.7 worker protocol/`ProcessExecutor`), RFC-0005 (quality, AC-8), RFC-0006 (import), RFC-0007 (feature capability, adapter interface)
- `docs/PPS-0001-platform-principles.md` (§5.3 artifact types, §5.8 canonical names)
- `docs/specifications/reconstruction-pipeline.md` (§2.3 SfM stage, §5 failure semantics), `docs/specifications/adaptive-engine.md` (§4, referenced for AI-prior placement)
- ADR-004, ADR-005, ADR-006, ADR-010, ADR-011, ADR-012, ADR-020, ADR-021, ADR-026, ADR-028, ADR-031, ADR-033, ADR-034, ADR-035, ADR-038
- CONSTITUTION.md §2 (protected surfaces), §3 (backend choice), §5 (change control)
- `THIRD_PARTY.yml`, `MODEL_LICENSES.yml` (dispositions, commit `2b978cf`)
- `engine/workers/process_executor.h`, `engine/workers/worker_handle.h` (execution seam)
