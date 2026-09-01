# P3 Milestone Status & Verification Index

**Status:** Living document
**Updated:** 2026-08-31

Tracks implementation milestone acceptance for the P3 trajectory / pose-graph / loop-closure work,
and an index of the corresponding verification reports. A milestone is ACCEPTED/COMPLETE only when
its verification report passes in both Debug and Release.

---

## Milestone Status

| Milestone | Status | Verification |
|-----------|--------|--------------|
| P2.3 Feature Extraction | ✅ COMPLETE | `feature` payload + FeatureArtifact + feature_sets |
| P2.5 Canonical Reconstruction | ✅ COMPLETE | — |
| P3 PoseGraph architecture (design + types) | ✅ COMPLETE | `P3-trajectory-pose-graph-loop-closure.md` (accepted) |
| P3-impl-6b GTSAM optimizer adapter | ✅ COMPLETE | `P3-impl-6b-verification-report.md` (537/537) |
| ADR-P3-GTSAM-001 GTSAM acceptance | ✅ ACCEPTED | `docs/architecture/ADR-P3-GTSAM-001.md` |
| **P3-impl-6c synthetic loop-closure → GTSAM E2E** | ✅ **ACCEPTED/COMPLETE** | `P3-impl-6c-verification-report.md` (537/537) |
| **P3-impl-7a visual loop candidate generation** | ✅ **ACCEPTED/COMPLETE** | `P3-impl-7a-verification-report.md` (549/549) |
| **7a.1 loop-closure capability executor integration** | ⏳ **DEFERRED debt** (not 7a defect) | — |
| **P3-impl-7b geometric loop verification** | ✅ **ACCEPTED/COMPLETE** | `P3-impl-7b-verification-report.md` (564/564) |
| P3-impl-7c visual loop → GTSAM E2E | ⏳ **← next** | — |
| Multi-session registration | ⏳ | — |
| LiDAR fusion | ⏳ | — |
| Dense reconstruction | ⏳ | — |
| AI / neural reconstruction | ⏳ | — |

---

## P3-impl-6c — Acceptance Record

- **Verdict:** PASS / ACCEPTED.
- **Deliverables:**
  - `core/trajectory/pose_graph_helpers.h` (new, header-only): canonical transform math,
    info-matrix validation, pose-graph assembly, loop-closure pipeline.
  - `tests/unit/test_gtsam_adapter.cpp` extended to 39 tests (+6 helper, +2 E2E).
  - `docs/architecture/P3-impl-6c-loop-closure-gtsam.md`, verification report.
- **Key proved result:** verified loop closure → canonical PoseGraph → real GTSAM LM reduces
  closed-square endpoint drift **0.200 m → 0.012 m (≈94%)**; original Trajectory unmutated
  (SHA-256 verified); optimization result + provenance persist to the CAS store.
- **Guardrails met:** loop reaches GTSAM (not a mock); output not fabricated; reduction not
  hard-coded; no new architectural surface (protected contracts unchanged; GTSAM stays isolated in
  the adapter).

### Proven end-to-end contour (the first one)

```text
Canonical Trajectory
      ↓
PoseGraph Assembly
      ↓
Loop Closure Candidate
      ↓
Verification
      ↓
Loop Closure Edge
      ↓
GTSAM
      ↓
Optimized Trajectory
      ↓
CAS + Provenance
```

---

## P3-impl-7a — Acceptance Record

- **Verdict:** PASS / ACCEPTED / COMPLETE (2026-08-31).
- **Deliverables:**
  - `core/loop_closure/feature_matcher.h` (backend-independent matching contract).
  - `core/loop_closure/loop_closure_candidate_gen.{h,cpp}` (windowed candidate generation +
    FeatureArtifact→canonical-set loading).
  - `adapters/visual_matching/` (`L2NearestMatcher`, classical deterministic matcher; std-only,
    no OpenCV/FLANN).
  - `engine/pipeline/loop_closure_detection.{h,cpp}` (candidate producer + capability registration).
  - `tests/unit/test_loop_closure_detection.cpp` (12 tests).
  - `docs/architecture/P3-impl-7a-verification-report.md`.
- **Proved:** FeatureArtifact → descriptor matching → score → ranking → temporal exclusion →
  `LoopClosureCandidate` → CAS + provenance. **Debug 549/549 · Release 549/549**;
  `spatial_gtsam_tests` unchanged at 39/39.
- **Boundary preserved:** `mock_16` is for deterministic tests only, NOT a production descriptor.
  Loop closure stays an observation/constraint, never a direct pose mutation. Matcher behind the
  adapter boundary; Core backend-free; GTSAM boundary unchanged (no GTSAM includes/uses in 7a code).

> **Stated capability consequence (correct, not overstated):** "Visual loop-closure **candidate-
> generation contract** implemented and verified" — NOT "production visual loop closure". Real
> descriptor quality on real images is unproven.

---

## P3-impl-7b — Acceptance Record

- **Verdict:** PASS / ACCEPTED / COMPLETE (2026-09-01).
- **Deliverables:**
  - `core/loop_closure/geometric_verifier.h`, `verification_options.h`,
    `correspondence_reconstruction.h` (backend-independent verification contract + configurable
    acceptance thresholds).
  - `core/loop_closure/feature_matcher.h` (additive: keypoints + `MatchCorrespondences` seam —
    no existing signature changed).
  - `adapters/visual_geometry/fundamental_verifier.{h,cpp}` (deterministic uncalibrated Fundamental
    F + RANSAC; fixed seed; no metric pose fabricated — `has_relative_pose` always false).
  - `engine/pipeline/loop_closure_verification.{h,cpp}` (orchestration: resolve artifacts →
    correspondences → verify → persist accepted AND rejected `LoopClosure` rows + CAS payload +
    manifest provenance).
  - `tests/unit/test_loop_closure_verification.cpp` (15 tests, incl. the two golden safety proofs).
  - `docs/architecture/P3-impl-7b-verification-report.md`.
- **Proved:** `Candidate ≠ LoopClosure` — a visually-similar but geometrically-wrong candidate is
  REJECTED; RANSAC recovers the true model under 70% outlier contamination; identical inputs give
  identical results; missing/malformed input fails closed with a typed error. The 6c synthetic
  verification stand-in is replaced with genuine geometric verification.
  **Debug 564/564 · Release 564/564**; `spatial_gtsam_tests` unchanged at 39/39.
- **Governance:** 6/7 gates PASS; `check_domain_types` FAIL is pre-existing 6c debt
  (`pose_graph_helpers.h:48,69`), documented in the report and not modified in 7b scope.
- **Boundary preserved:** no GTSAM/PoseGraph/7c work; `LoopClosure` stays an observation (no
  transform/covariance); Core backend-free; schemas/protected contracts untouched.

---

## Deferred debt — P3-impl-7a.1 Loop Closure Capability Executor Integration (OPEN)

Deliberately deferred, NOT a defect of P3-impl-7a:

- `"loop_closure"` is registered as a **declarative** pipeline stage and the producer
  (`DetectLoopClosureCandidates`) is tested and callable, but the full `Engine::RunPipeline`
  executor path is **not bound** to the capability.
- Why: the in-process executor dispatches a single worker `task_type` per stage over a single
  input ref, and `DemoWorkerProfile().capabilities` lists only
  `feature_extraction, reconstruction, validation`. Candidate generation consumes **multiple**
  FeatureArtifacts per run, requiring a new worker `task_type` + capability-profile wiring +
  multi-input handling — a separate integration increment touching protected worker/task
  infrastructure.
- Target (do NOT implement automatically):
  `Engine → Scheduler → Worker → loop_closure capability → FeatureArtifacts → candidate generator
  → LoopClosureCandidate → ArtifactStore`.

---

## Next step

**P3-impl-7c — Visual Loop → GTSAM E2E.** Consume 7b's **accepted** `LoopClosure` records and
produce `PoseGraphEdge` constraints (`BuildLoopClosureEdge`, 6c scaffolding) into a PoseGraph, then
optimize via the existing GTSAM adapter. Do **not** start automatically; requires explicit
authorization.

