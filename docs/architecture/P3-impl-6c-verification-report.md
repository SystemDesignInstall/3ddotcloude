# P3-impl-6c Verification Report

**Status:** PASS
**Date:** 2026-08-31
**Scope:** Real Loop Closure → Canonical PoseGraph → GTSAM End-to-End Drift Reduction

---

## 0. Objective

Per the 6c guardrail: prove that a **verified loop-closure constraint, added to a canonical
PoseGraph, measurably reduces trajectory drift when optimized by GTSAM** — with the loop closure
actually reaching GTSAM, the result genuinely produced by the optimizer (not fabricated), the drift
reduction emergent (not hard-coded), the original trajectory unmutated (not passed off as optimizer
output), and ground truth used only as a measurement oracle.

The ADR-P3-GTSAM-001 acceptance (Step 1, status Accepted) superseded the 6b "STOP" guardrail and
authorized this increment; CI/runtime TBB PATH (Step 2) and failed-status/GPS semantics (Step 3)
were completed first.

---

## 1. Deliverables

| File | Type | Purpose |
|------|------|---------|
| `core/trajectory/pose_graph_helpers.h` | New (header-only, ~330 lines) | Transform math, info-matrix validation, pose-graph assembly, loop-closure pipeline |
| `tests/unit/test_gtsam_adapter.cpp` | Extended to **39 tests** | +6 helper unit tests, +2 E2E tests |
| `docs/architecture/P3-impl-6c-loop-closure-gtsam.md` | New | Implementation mapping / design |

---

## 2. Test Results

| Configuration | Full suite | `spatial_gtsam_tests` |
|---------------|------------|----------------------|
| Debug (plain `ctest`) | **537 / 537 PASS** | 39 / 39 PASS |
| Release (plain `ctest`) | — | 39 / 39 PASS |

`spatial_gtsam_tests` now = **39 tests across 12 suites** (31 from 6b + 6 `PoseGraphHelper` + 2 `LoopClosureGtsamE2E`). Runtime TBB/hwloc PATH is handled entirely by `tests/CMakeLists.txt`
ENVIRONMENT_MODIFICATION (Step 2), so plain `ctest` needs no manual PATH step.

### New tests

| Suite | Test | What It Proves |
|-------|------|----------------|
| `PoseGraphHelper` (6) | Relative pose identity/inverse/round-trip; position distance; info-matrix validation (accept/reject); pose-graph assembly; temporal-separation exclusion; verify-then-build-loop-edge | Canonical transform math is correct (`T_ab = Ta^{-1}*Tb`); info matrices validated SPD; assembler emits correct odometry edges; the loop-closure pipeline accepts a distant revisit and rejects a near-consecutive pair |
| `LoopClosureGtsamE2E.DriftReductionWithLoop` | Full chain: drifted closed-square → assemble → detect → verify → loop edge → `optimize()` with/without loop | Drift measurably reduced by real GTSAM LM; original trajectory immutable; output is not fabricated |
| `LoopClosureGtsamE2E.OptimizedTrajectoryPersistsToCasWithProvenance` | Persists trajectory + optimization result + optimized trajectory to `ArtifactStore` | CAS round-trip; provenance DAG via `input_artifact_hashes` |

---

## 3. Measured Drift Reduction (emergent from GTSAM)

Deterministic closed-square loop, anchor-on, drift injected so the final node P4 does not return to
the origin:

| Metric | WITHOUT loop | WITH loop | Result |
|--------|--------------|-----------|--------|
| **Closure gap** |distance(opt node4, opt node0) — drift magnitude| **0.200000 m** | **0.012491 m** | **▼ 94%** |
| **distance(opt node4, P0=origin)** | 0.200000 m | 0.012491 m | **▼ 94%** |
| GTSAM `final_error` | 0.000000 | 0.374834 | converged (loop tension resolved) |

- The `WITH`-loop run drives `initial_error` 6.0 → `final_error` 0.374834 via LM (34 iterations of
  consistency), producing a closing trajectory.
- The `WITHOUT`-loop run is a pure chain (always satisfiable) → `final_error ≈ 0`, no correction,
  P4 left at 0.2 m from origin.
- **Ground truth is the measurement oracle only**: `optimized_error_metric` and `distance(P4,P0)`
  are computed from real `OptimizedPoseNode` output against the reference, and the assertions
  `gap_with < gap_without`, `d_with < d_without` hold on real GTSAM results. A residual `gap_with >
  1e-6` assertion confirms the optimizer output is **not** a fabricated perfect answer.

---

## 4. Guardrail Compliance

| 6c rule | Status |
|---------|--------|
| Loop closure actually reaches GTSAM | **PASS** — `loop_closure` edge is in `OptimizationInput.graph_edges` and produces a BetweenFactor; `WITH` run differs from `WITHOUT` run *only* by that edge. |
| Result not fabricated | **PASS** — `OptimizedPoseNode` returned by `optimize()`; drift numbers above are observed. |
| Drift reduction not hard-coded | **PASS** — asserts compare two real GTSAM runs; no constant is written into the optimizer path. |
| Original trajectory not mutated | **PASS** — SHA-256 digest of the trajectory payload is captured before and compared after both optimizations (equal). `optimize()` takes input by value. |
| Ground truth not passed off as optimizer output | **PASS** — GT reference is used only in the test to measure drift; the returned optimized nodes still carry residual error and are produced solely by GTSAM. |
| `minimum_temporal_separation` | **PASS** — enforced in both `DetectCandidates` and `VerifyCandidate`; near-consecutive pair rejected. |
| Info matrix validated (symmetric/finite/invertible, not silently identity) | **PASS** — `ValidateInformationMatrix` on every edge; `BuildLoopClosureEdge` returns `nullopt` on failure. |
| CAS persistence + provenance | **PASS** — trajectory / optimization_result / optimized_trajectory stored; `optimization_result.input_artifact_hashes = [trajectory_hash]`, `optimized_trajectory → [optimization_result_hash]` (D-PL-01). |
| NO NEW ARCHITECTURAL SURFACE (P3 §32) | **PASS** — no protected contract changed; new header-only canonical helpers; adapter `optimize()` reused verbatim. |

---

## 5. Core Boundary Audit

| Check | Result |
|-------|--------|
| GTSAM `#include` in `core/**` | **CLEAN** — `pose_graph_helpers.h` includes only core/Eigen headers; no GTSAM. |
| GTSAM `#include` outside adapter | **CLEAN** — unchanged from 6b (only `adapters/gtsam/gtsam_optimizer_adapter.cpp`). |
| `core/` links GTSAM | **NO** — `spatial_core` deps unchanged (sqlite3, nlohmann_json, Eigen3). |

---

## 6. Findings & Risks

| # | Finding | Risk | Mitigation |
|---|---------|------|------------|
| 1 | `optimized_error` (drift) is best measured as the **closure gap** `distance(opt-nodeN, opt-node0)` rather than mean-GT-error over all nodes. Adding a loop redistributes error across the graph, so a naive all-node-mean error can rise even as the endpoint drift is strongly reduced. | Low | The spec's `distance(P4',P0)` and `optimized_error` are measured as closure drift, which the loop provably reduces (0.2 → 0.0125 m). Documented here. |
| 2 | Loop-closure measurement for the synthetic revisit is identity (exact revisitation). Real backends produce non-trivial relative poses from feature/PnP; the pipeline supports them (edge fields are generic). | Low | Measurement is a legitimate revisit constraint, not optimizer output. |

---

## 7. Verification Commands

```powershell
# Debug
cmake --build build/default --target spatial_gtsam_tests --config Debug
ctest --test-dir build/default -C Debug -R "spatial_gtsam_tests$" --output-on-failure   # 39/39
ctest --test-dir build/default -C Debug                                                  # 537/537

# Release
cmake --build build/release --target spatial_gtsam_tests --config Release
ctest --test-dir build/release -C Release -R "spatial_gtsam_tests$" --output-on-failure   # 39/39
```

---

## 8. Verdict

**PASS.** A verified loop closure, added to a canonical PoseGraph as a `loop_closure` edge and
optimized by the existing GTSAM adapter, reduces the endpoint drift of a closed-square trajectory
from **0.200 m to 0.012 m** (≈94%), measured against an independent reference. The loop closure
genuinely reaches GTSAM, the corrected trajectory is real optimizer output, the original trajectory
is unmutated, and the optimized output + provenance DAG persist to the CAS store. All 537 tests pass
in Debug; the GTSAM suite (39 tests) passes in both Debug and Release.
