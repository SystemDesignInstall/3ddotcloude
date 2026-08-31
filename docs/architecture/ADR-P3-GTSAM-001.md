# ADR-P3-GTSAM-001 — GTSAM as Primary Pose-Graph Optimization Backend

**Status:** Accepted
**Date:** 2026-08-31
**Scope:** Architectural decision (approval to proceed with P3-impl-6c)
**Depends on:** P3-impl-6b COMPLETE (optimizer adapter + tests, 537/537 PASS), P3 trajectory/pose-graph/loop-closure design COMPLETE
**Supersedes:** No prior ADR file existed (previous ADR references in doc comments were never materialized as standalone files)

---

## 1. Context

Spatial Platform must correct odometry drift in long camera trajectories by optimizing a canonical
`PoseGraph` (nodes + odometry edges + loop-closure edges + prior/GPS edges) into corrected poses.
This requires a nonlinear least-squares pose-graph optimizer.

The originally planned optimizer (referenced across P3 design docs as "ADR-005: GTSAM") has now been
**implemented and test-verified** behind an adapter seam (`adapters/gtsam`), alongside the canonical
`PoseGraph`, `OptimizationResult`, and `LoopClosure` data model (P3-impl-6b). This ADR codifies the
accepted architectural decision so that P3-impl-6c (real loop closure → PoseGraph → GTSAM E2E +
proof of drift reduction) can proceed.

## 2. Decision

**G1 — GTSAM is the primary production optimization backend.**

GTSAM (Georgia Tech Smoothing and Mapping) is the **primary production backend** for canonical
PoseGraph global optimization. It is used **only through the adapter boundary**
(`adapters/gtsam/gtsam_optimizer_adapter`).

**G2 — GTSAM is replaceable.**

GTSAM is **not** fused into Core. It sits behind a canonical capability / optimizer-seam equivalent
(`OptimizationInput` → `OptimizationResult`). A future alternative backend (e.g. Ceres) can replace
GTSAM without changing Core, the data model, or the coordinate-frame semantics.

**G3 — GTSAM's ownership is strictly limited to optimization mathematics.**

GTSAM owns:
- nonlinear factor-graph construction and Levenberg-Marquardt / ISAM-style optimization;
- canonical pose-graph global optimization (node correction, error reduction);
- loop-closure factors, priors, GPS factors (as `PriorFactor<Pose3>`), IMU preintegration factors;
- uncertainty / covariance propagation from all constraints;
- future sensor-fusion factors (LiDAR, GNSS, IMU, GCP).

The adapter consumes **canonical** `PoseGraph` and produces **canonical** `OptimizationResult` +
`OptimizedTrajectory` payloads. GTSAM types (`::gtsam::NonlinearFactorGraph`, `::gtsam::Values`,
`::gtsam::Pose3`, ...) never cross the adapter boundary into Core.

**G4 — GTSAM does NOT own the platform data model.**

GTSAM does **not** own or modify:
- Scene / data-model semantic ownership;
- frame / coordinate-frame semantics (`T_trajectory_camera`, scalar-last quaternion, ADR-007 ordering);
- feature extraction / matching;
- loop detection (candidate generation);
- image processing;
- point-cloud registration (TEASER++, Open3D, KISS-ICP);
- scene storage, artifact storage (CAS), or metadata DB;
- AI / learned components;
- multi-session alignment.

All of the above remain canonical Core concerns. GTSAM is purely the optimization backend.

## 3. Architectural Boundary

```
                       canonical Core (data model + semantics)
                                    │
        PoseGraph (canonical)  OptimizationInput (canonical)
                                    │
          ┌─────────────────────────▼─────────────────────────┐
          │        optimization capability / seam             │
          │        (backend-independent contract)             │
          └─────────────────────────┬─────────────────────────┘
                                    │  adapters/gtsam (only GTSAM includes live here)
                                    ▼
                        GTSAM (optimization math only)
                                    │
          ┌─────────────────────────▼─────────────────────────┐
          │  OptimizationResult (canonical)                   │
          │  OptimizedTrajectory (new CAS artifact, canonical)│
          └───────────────────────────────────────────────────┘
```

## 4. Forbidden / Reserved Types

**Forbidden in `core/` and any non-adapter code:**
- `::gtsam::Pose3`, `::gtsam::Value`, `::gtsam::Values`
- `::gtsam::NonlinearFactorGraph`, `::gtsam::NonlinearFactor`
- `::gtsam::PriorFactor`, `::gtsam::BetweenFactor`
- `::gtsam::LevenbergMarquardtOptimizer`, `::gtsam::ISAM2`, `::gtsam::ISAM2Params`

**Allowed ONLY inside `adapters/gtsam/` (native GTSAM types):**
- the full GTSAM API, always referenced with `::gtsam::` prefix when inside
  `namespace spatial::adapters::gtsam` (to avoid resolving `gtsam::gtsam::` to the local namespace).

## 5. Consequences

Positive:
- Correct, globally optimized trajectories with verified drift reduction (P3-impl-6c).
- Clean backend seam; Ceres can replace GTSAM later without touching Core.
- Uncertainty propagation and sensor fusion are delegated to a mature library.

Negative / constraints:
- GTSAM is a heavyweight third-party dependency (static lib + Conan TBB runtime dependency).
- The adapter boundary must be strictly enforced; GTSAM types must never leak into Core.
- Coordinate-frame and data-model semantics must remain canonical and GTSAM-agnostic.

## 6. Normative Decisions

| ID | Decision |
|----|----------|
| G1 | GTSAM is the primary production pose-graph optimization backend. |
| G2 | GTSAM is replaceable via the canonical optimization seam (future Ceres). |
| G3 | GTSAM owns factor-graph construction, pose-graph optimization, loop-closure/prior/GPS/IMU factors, and uncertainty propagation. |
| G4 | GTSAM does NOT own Scene/data model/coordinate frames/feature/loop-detection/storage/AI/multi-session. |

## 7. Related

- P3-impl-6b verification report (537/537 PASS) — optimizer adapter + tests.
- `docs/architecture/P3-trajectory-pose-graph-loop-closure.md` — canonical data model (D-PG-*, D-LC-*, D-OPT-*).
- `docs/architecture/P3-impl-6-implementation-mapping.md` — implementation mapping.
