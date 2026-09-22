# P3 Milestone Status & Verification Index

**Status:** Living document
**Updated:** 2026-09-16

Tracks implementation milestone acceptance for the P3 trajectory / pose-graph / loop-closure work,
and an index of the corresponding verification reports. A milestone is ACCEPTED/COMPLETE only when
its verification report passes in both Debug and Release.

---

## Milestone Status

| Milestone | Status | Verification |
|-----------|--------|--------------|
| P2.3 Feature Extraction | тЬЕ COMPLETE | `feature` payload + FeatureArtifact + feature_sets |
| P2.5 Canonical Reconstruction | тЬЕ COMPLETE | тАФ |
| P3 PoseGraph architecture (design + types) | тЬЕ COMPLETE | `P3-trajectory-pose-graph-loop-closure.md` (accepted) |
| P3-impl-6b GTSAM optimizer adapter | тЬЕ COMPLETE | `P3-impl-6b-verification-report.md` (537/537) |
| ADR-P3-GTSAM-001 GTSAM acceptance | тЬЕ ACCEPTED | `docs/architecture/ADR-P3-GTSAM-001.md` |
| **P3-impl-6c synthetic loop-closure тЖТ GTSAM E2E** | тЬЕ **ACCEPTED/COMPLETE** | `P3-impl-6c-verification-report.md` (537/537) |
| **P3-impl-7a visual loop candidate generation** | тЬЕ **ACCEPTED/COMPLETE** | `P3-impl-7a-verification-report.md` (549/549) |
| **7a.1 loop-closure capability executor integration** | тП│ **DEFERRED debt** (not 7a defect) | тАФ |
| **P3-impl-7b geometric loop verification** | тЬЕ **ACCEPTED/COMPLETE** | `P3-impl-7b-verification-report.md` (564/564) |
| **P3.1 production sparse-correction pipeline** | **INCOMPLETE** | `P3.1-production-sparse-correction-verification-report.md`; metric-source gate unresolved; 676/676 is historical partial-pipeline evidence |
| P3-impl-7c visual loop тЖТ GTSAM E2E | тП│ **тЖР next** | тАФ |
| Multi-session registration | тП│ | тАФ |
| LiDAR fusion | тП│ | тАФ |
| Dense reconstruction | тП│ | тАФ |
| AI / neural reconstruction | тП│ | тАФ |

---

## P3-impl-6c тАФ Acceptance Record

- **Verdict:** PASS / ACCEPTED.
- **Deliverables:**
  - `core/trajectory/pose_graph_helpers.h` (new, header-only): canonical transform math,
    info-matrix validation, pose-graph assembly, loop-closure pipeline.
  - `tests/unit/test_gtsam_adapter.cpp` extended to 39 tests (+6 helper, +2 E2E).
  - `docs/architecture/P3-impl-6c-loop-closure-gtsam.md`, verification report.
- **Key proved result:** verified loop closure тЖТ canonical PoseGraph тЖТ real GTSAM LM reduces
  closed-square endpoint drift **0.200 m тЖТ 0.012 m (тЙИ94%)**; original Trajectory unmutated
  (SHA-256 verified); optimization result + provenance persist to the CAS store.
- **Guardrails met:** loop reaches GTSAM (not a mock); output not fabricated; reduction not
  hard-coded; no new architectural surface (protected contracts unchanged; GTSAM stays isolated in
  the adapter).

### Proven end-to-end contour (the first one)

```text
Canonical Trajectory
      тЖУ
PoseGraph Assembly
      тЖУ
Loop Closure Candidate
      тЖУ
Verification
      тЖУ
Loop Closure Edge
      тЖУ
GTSAM
      тЖУ
Optimized Trajectory
      тЖУ
CAS + Provenance
```

---

## P3-impl-7a тАФ Acceptance Record

- **Verdict:** PASS / ACCEPTED / COMPLETE (2026-08-31).
- **Deliverables:**
  - `core/loop_closure/feature_matcher.h` (backend-independent matching contract).
  - `core/loop_closure/loop_closure_candidate_gen.{h,cpp}` (windowed candidate generation +
    FeatureArtifactтЖТcanonical-set loading).
  - `adapters/visual_matching/` (`L2NearestMatcher`, classical deterministic matcher; std-only,
    no OpenCV/FLANN).
  - `engine/pipeline/loop_closure_detection.{h,cpp}` (candidate producer + capability registration).
  - `tests/unit/test_loop_closure_detection.cpp` (12 tests).
  - `docs/architecture/P3-impl-7a-verification-report.md`.
- **Proved:** FeatureArtifact тЖТ descriptor matching тЖТ score тЖТ ranking тЖТ temporal exclusion тЖТ
  `LoopClosureCandidate` тЖТ CAS + provenance. **Debug 549/549 ┬╖ Release 549/549**;
  `spatial_gtsam_tests` unchanged at 39/39.
- **Boundary preserved:** `mock_16` is for deterministic tests only, NOT a production descriptor.
  Loop closure stays an observation/constraint, never a direct pose mutation. Matcher behind the
  adapter boundary; Core backend-free; GTSAM boundary unchanged (no GTSAM includes/uses in 7a code).

> **Stated capability consequence (correct, not overstated):** "Visual loop-closure **candidate-
> generation contract** implemented and verified" тАФ NOT "production visual loop closure". Real
> descriptor quality on real images is unproven.

---

## P3-impl-7b тАФ Acceptance Record

- **Verdict:** PASS / ACCEPTED / COMPLETE (2026-09-01).
- **Deliverables:**
  - `core/loop_closure/geometric_verifier.h`, `verification_options.h`,
    `correspondence_reconstruction.h` (backend-independent verification contract + configurable
    acceptance thresholds).
  - `core/loop_closure/feature_matcher.h` (additive: keypoints + `MatchCorrespondences` seam тАФ
    no existing signature changed).
  - `adapters/visual_geometry/fundamental_verifier.{h,cpp}` (deterministic uncalibrated Fundamental
    F + RANSAC; fixed seed; no metric pose fabricated тАФ `has_relative_pose` always false).
  - `engine/pipeline/loop_closure_verification.{h,cpp}` (orchestration: resolve artifacts тЖТ
    correspondences тЖТ verify тЖТ persist accepted AND rejected `LoopClosure` rows + CAS payload +
    manifest provenance).
  - `tests/unit/test_loop_closure_verification.cpp` (15 tests, incl. the two golden safety proofs).
  - `docs/architecture/P3-impl-7b-verification-report.md`.
- **Proved:** `Candidate тЙа LoopClosure` тАФ a visually-similar but geometrically-wrong candidate is
  REJECTED; RANSAC recovers the true model under 70% outlier contamination; identical inputs give
  identical results; missing/malformed input fails closed with a typed error. The 6c synthetic
  verification stand-in is replaced with genuine geometric verification.
  **Debug 564/564 ┬╖ Release 564/564**; `spatial_gtsam_tests` unchanged at 39/39.
- **Governance:** 6/7 gates PASS; `check_domain_types` FAIL is pre-existing 6c debt
  (`pose_graph_helpers.h:48,69`), documented in the report and not modified in 7b scope.
- **Boundary preserved:** no GTSAM/PoseGraph/7c work; `LoopClosure` stays an observation (no
  transform/covariance); Core backend-free; schemas/protected contracts untouched.

---

## P3.1 тАФ Current Record

- **Verdict:** INCOMPLETE (2026-09-12). The earlier COMPLETE declaration is withdrawn against the original full-chain GO, without changing its scope.
- **Implemented subset:** registered `p3_sparse_correction`, two-stage dispatch, per-stage cache policy, sparse worker/passthrough and host commit/BA route. CLI reaches `RunPipeline`, but injects empty providers and returns zero for a failed manifest.
- **Historical tests:** 676/676 Debug and Release, including six partial-pipeline tests with a BA stub. These results do not prove calibrated pose inference, geometric BA improvement or the missing production stages.
- **Phase 1 finding:** Fundamental verification has no relative pose. The existing unit-direction scale resolver requires a measured calibrated direction plus a genuine metric trajectory; neither declaration strings nor GTSAM graph optimization generates the missing image measurement. A compliant calibrated provider remains unresolved.
- **Required work remains:** LC/metric pose/PoseGraph/GTSAM/feedback/retriangulation through the runner, measured BA golden, actual providers in CLI, true content-mismatch N3, N4/N6, deterministic double-run and transaction/replay evidence. None is reclassified as optional or a later milestone.
- **Current evidence:** `P3.1-production-sparse-correction-closure.md` section 6 and `P3.1-production-sparse-correction-verification-report.md` (original AC-01..AC-31).
- **Scope:** no Phase 2 runtime edits during the metric-source audit; no changes to frozen 8a/8b/8c math, no migration, no 8d/dense/AI implementation.
- **Step 8 (retriangulation, PROVEN):** new tests RT1тАУRT8 in `tests/unit/test_p3_trajectory_optimization.cpp` (18/18 in `spatial_p3_trajectory_optimization_tests`, serial ctest #710). RT1 proves real geometry fix-up: committed DLT points under drifted poses have mean 3D error > 5 cm vs retriangulated v3 points < 2 cm, with all rows REPLACED. RT2 supersede + monotonic `created_at_ns` (+100 clock advance); RT3 verbatim image/camera/track lineage + revision lineage with applied-revision id and input-artifact hashes; RT4 canonical observation resolution from registered `feature_sets` artifacts; RT5/RT6/RT7 fail closed (missing feature set, single-element track, out-of-range `point2d_idx`) leaving the applied revision unchanged; RT8 vacuous (no points тЖТ no v3). Full suite **Debug 712/712** and **Release 712/712** (fresh `build/release` tree, GTSAM ON; 90 s). Gates 7/7 PASS (`check_dependencies`, `check_schemas`, `check_worker_boundary`, `check_arch_debt`, `check_domain_types`, `check_rfc`, `check_constitution --base HEAD --rfc RFC-0000`); `git diff --check` clean; frozen 8a/8b/8c math files untouched vs HEAD.
- **Step 9 (Bundle Adjustment, PROVEN):** real GTSAM BA over the injected `ReconstructionOptimizer` seam. Adapter suite GA1тАУGA6 in `tests/unit/test_gtsam_bundle_adjustment.cpp` (`spatial_gtsam_adapter_tests`): GA1/GA2 real LM refinement reduces reprojection residual and moves poses; GA3 deterministic for fixed seed; GA4 fail-closed without observations; GA5 refuses non-LM optimizer; GA6 empty observations are a well-defined no-op. Runner-level BT1 in `test_p3_trajectory_optimization.cpp`: committedтЖТappliedтЖТretriangulated v3 chain, all superseded, v4 the sole succeeded revision, provably v3 superseded in the same transaction and (+100 ns) v4 timing, `nlohmann`-parsed optimizer telemetry asserts `converged=true` and `rms_after < rms_before`; validation via index-aligned `MeanDistanceToTruth` (id-keyed lookup is invalid because `Retriangulate` renumbers replaced rows). Runner Step-9 block `sparse_correction_runner.cpp:1704` chain-compacts the committed revision exactly once before BA; observations resolved STRICTLY from the corrected chain (v3) frameтЖТfeature linkage; the plain committed-only path (golden images-mode E2E) keeps the milestone-approved EMPTY observation list (`P3-production-e2e-verification-report.md` ┬з"runner passes an empty observation list") тАФ an unresolvable-observation throw on that path regressed golden test #585 and is fixed. Suite verified with real GTSAM (not stub): **Debug 713/713** and **Release 713/713**.
- **Step 10 (Production visual-loop тЖТ GTSAM E2E, PROVEN):** the SAME chain, entered ONLY through `Engine::RunPipeline("p3_sparse_correction", тАж)` тАФ one invocation per case, no direct host-stage calls. New `E2E1_FullProductionChainSingleInvocation` / `E2E2_NoMetricBasisYieldsNoCorrection` in `RealGtsamBundleAdjustmentTest`; all seams REAL (`L2NearestMatcher` + `EssentialGeometricVerifier` + `GtsamTrajectoryOptimizer` + `GtsamBundleAdjustmentOptimizer`). E2E1 (valid-basis ╬╗=7 noise 0.4 px): closure accepted with `has_relative_pose` at the ~0.65 m baseline; PoseGraph edge is the verbatim measurement; real GTSAM converged + error reduced; node pulled тЙе5 cm off the drift; applied revision via per-frame FrameID join toward truth in BOTH translation (<5 cm) and rotation (closer to truth than the drifted input); retriangulated v3 recomputed the geometry (images/cameras/tracks verbatim) toward truth (mean 0.85 m vs committed 1.99 m тАФ the exact-pixel tightness <2 cm is RT1's separate claim); real BA v4 (LM, `rms_after < rms_before`, +100 ns, cameras verbatim, geometry changed); persistence: committed/applied/v3 superseded, v4 the SOLE succeeded+latest; feature artifacts untouched. E2E2 (basis `declared=false`): manifest FAILS (stage 0 succeeded, stage 1 failed via the D5 gate over the GA6 empty-observation no-op with the REAL BA seam); the closure is ACCEPTED with inliers but visual-only (`has_relative_pose=false`) тАФ INV-3: NO PoseGraph, NO optimization (real GTSAM never invoked), NO applied, NO v3, NO v4; the committed reconstruction is the ONLY revision, byte-identical to the CAS input. Scheduler note (pre-existing, `test_p3_runner_context.cpp:1261-1275`): a failed stage is retried while the error is recoverable (the in-process executor marks every runner exception recoverable), so loop-closure evidence may accumulate across the retries; every instance is asserted accepted + visual-only. Suite #710 (`spatial_p3_trajectory_optimization_tests`) **21/21** Debug + Release with real GTSAM. **Step 10 E2E target: PASS.** **Full suite: NOT flat-green тАФ known pre-existing MetadataDb concurrency flakes** (Debug 710/713, Release 711/713; only `MetadataDbTest` cases CreateAppliesMigrations / ReadOnlyRejectsWrites / FindOrCreateSceneCreatesOnce / RegisterSensorRoundTrip / ReadSurfaceResolvesSessionsAndScenes fail, differing subsets per run); **isolated 24/24** (`MetadataDbTest.*` passes fully in `spatial_unit_tests`), unrelated to this change (different test binary; only `test_p3_trajectory_optimization.cpp` was edited). `git diff --check` clean.
- **Step 11 (failure/recovery N1тАУN6 through `Engine::RunPipeline` ONLY, PROVEN):** the milestone's negative/recovery acceptance, all entered through the same surface with NO direct host-stage calls. **N1 damaged SHA-256** (`N1_DamagedSha256FailsClosedAndQuarantines`): a real, previously-Put CAS payload whose bytes are corrupted under the same content hash fails the read re-verification тАФ manifest "failed", the corrupt payload QUARANTINED and its index row flagged `validation_status="degraded"`, nothing committed (no scene, no revision). **N2/N3/N5 inputs** (existing N1NonImprovingGateFailsThroughSurface / N2MissingInputFailsClosedAndNothingCommitted / N3NonSucceededPayloadFailsClosed / N5UnpinnedSeedFailsClosedBeforeAnyWrite) remain surface-level with `StubOptimizer`. **N3 mid-chain stage failure** (`N3_StageGateFailureRetainsV3NoV4`, trajectory suite): the full measured chain (LC тЖТ PoseGraph тЖТ real GTSAM trajectory optimization тЖТ apply тЖТ retriangulated v3 тЖТ chain-compaction of the committed revision) runs but the seam's D5 gate REFUSES the refinement (rms_after == rms_before): retriangulated v3 is RETAINED as a succeeded revision (never erased), the committed revision IS compacted, and NO v4 row/artifact exists under ANY seam attempt; every succeeded revision is a `spatial_retriangulator` v3, latest is a v3; the failed stage's only published artifacts are measurement evidence (accepted loop closure), never a reconstruction output. **N4 all-status newest preflight** (`N4_NonSucceededNewestRejectedBeforeAnyWrite`): a scene whose newest revision (ANY status) is `reconstructing` is refused by the runner `sparse_correction_runner.cpp:1617-1632` BEFORE any DB write (P12) тАФ the pre-seeded rows stay byte-identical, no closure/graph/result/revision evidence appears, `QueryLatest` correctly has no value. **N5 deterministic replay** (`N5_DeterministicReplayNeverDuplicatesRevisions`): identical re-run тЖТ same `pipeline_hash`, same pass-through output, duplicate committed identity refused fail-closed, the lineage stays exactly run1's (superseded + succeeded v4 byte-identical to run1's terminal CAS artifact). **N6 cache + rollback** (`N6_CommittedStageNeverReplayedAndRerunRollsBack`): the DB-committing correction stage is `CachePolicy::kNever` тАФ identical re-run replays the pure-CAS `sparse_reconstruction` stage from cache (`cache_hit=true`) but RE-EXECUTES the committing stage (`cache_hit=false`) and refuses again; the failed re-run wrote NOTHING тАФ same two revision rows, same succeeded latest, no partial/duplicate writes. New tests: `spatial_p3_sparse_correction_pipeline_tests` **29/29**, `spatial_p3_trajectory_optimization_tests` **22/22** (N3 added to the Step 10 21/21). Full batteries this run: **Debug 717/717** and **Release 717/717** (no MetadataDb flakes this run). `git diff --check` clean. Remaining P3.1 acceptance items: CLI real providers, AC-22 (NOT started per GO).
- **Step 12 (CLI real providers + CLI E2E through the SOLE user entry point, PROVEN):**  the CLI
  composition root injects the REAL seam implementations тАФ `L2NearestMatcher` +
  `EssentialGeometricVerifier` + `GtsamTrajectoryOptimizer` + `GtsamBundleAdjustmentOptimizer`
  (owned by the engine handle so the seam objects outlive the in-process runner's value-captured
  pointers; code committed at HEAD `fbef42b` alongside the adapter links in `cli/CMakeLists.txt`,
  the `--input <file>`/`--config <json>` run path, and the exit code тАФ `RunCommand` returns 0 ONLY
  on a succeeded manifest). GTSAM's runtime DLLs (tbb12/tbbmalloc) are copied post-build next to
  `spatial.exe` so every child-spawned CLI works without a manual PATH. New
  `tests/unit/test_cli_sparse_correction_e2e.cpp` (`spatial_cli_sparse_correction_e2e_tests`, 2
  tests, discovery) spawns the REAL binary `spatial run p3_sparse_correction --project <dir>
  --input <recon.json> --config <json>` and never calls internal stages directly:
  **positive** тАФ exit 0, manifest "succeeded" with BOTH stages succeeded, and the FULL v4 chain
  persisted (4 rows: committed `colmap` superseded and byte-identical to the submitted document,
  `spatial_optimizer` applied superseded, `spatial_retriangulator` v3 superseded with v3
  images/cameras verbatim, real-GTSAM `spatial_gtsam_bundle_adjuster` v4 the SOLE succeeded +
  latest with LM `rms_after < rms_before`, byte-identical intrinsics, `created_at_ns == v3 + 100`);
  the geometry really changed through the CLI (mean v3 error < committed error > 5 cm, v3 moved)
  and the GTSAM trajectory optimization converged with the node pulled тЙе 5 cm off the drift;
  **negative** тАФ a fail-closed config (missing `random_seed`, D6) yields exit code 1 with a
  "failed" manifest (stage 0 succeeded, stage 1 failed) and NO database writes (P12: no scene, no
  revision rows). The GTSAM LM damping diagnostic (`Partial Cholesky on HessianFactor failed`) is
  emitted to stdout before the manifest and is tolerated by the test's JSON scanner (identical to
  the in-process E2E behaviour). Full batteries **Debug 719/719** and **Release 719/719** (Step 11
  was 717/717; the +2 are the CLI E2E cases); the pre-existing CLI-spawn suites
  (`CliImportTest` 4/4, `CliFeatureExtractionTest` 4/4) still pass against the GTSAM-linked
  `spatial.exe` via the copied DLLs. Gates 7/7 PASS (`check_dependencies`, `check_schemas`,
  `check_worker_boundary`, `check_arch_debt`, `check_domain_types`, `check_rfc`, `check_constitution
  --base HEAD --rfc RFC-0000`); `git diff --check` clean (tracked diff = `tests/CMakeLists.txt`
  block activation only) and the new test file has no trailing whitespace. Remaining P3.1 items:
  AC-22 (PROVEN 2026-09-12: `AC22_DeterministicSecondRunThroughProductionSurface` twin тАФ two fresh
  `Engine::RunPipeline` invocations through REAL GTSAM seams, semantic equivalence asserted,
  N5 duplicate-refusal re-proven; Debug EXIT 0 Release EXIT 0; full trajectory 23/23 + CLI
  sparse-correction e2e 2/2 + production e2e 8/8 in both configs).

---

## Deferred debt тАФ P3-impl-7a.1 Loop Closure Capability Executor Integration (OPEN)

Deliberately deferred, NOT a defect of P3-impl-7a:

- `"loop_closure"` is registered as a **declarative** pipeline stage and the producer
  (`DetectLoopClosureCandidates`) is tested and callable, but the full `Engine::RunPipeline`
  executor path is **not bound** to the capability.
- Why: the in-process executor dispatches a single worker `task_type` per stage over a single
  input ref, and `DemoWorkerProfile().capabilities` lists only
  `feature_extraction, reconstruction, validation`. Candidate generation consumes **multiple**
  FeatureArtifacts per run, requiring a new worker `task_type` + capability-profile wiring +
  multi-input handling тАФ a separate integration increment touching protected worker/task
  infrastructure.
- Target (do NOT implement automatically):
  `Engine тЖТ Scheduler тЖТ Worker тЖТ loop_closure capability тЖТ FeatureArtifacts тЖТ candidate generator
  тЖТ LoopClosureCandidate тЖТ ArtifactStore`.

---

## Next step

**P3-impl-7c тАФ Visual Loop тЖТ GTSAM E2E.** Consume 7b's **accepted** `LoopClosure` records and
produce `PoseGraphEdge` constraints (`BuildLoopClosureEdge`, 6c scaffolding) into a PoseGraph, then
optimize via the existing GTSAM adapter. Do **not** start automatically; requires explicit
authorization.
