# ADR-005 — GTSAM as unified sensor factor graph

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

A single project may contain images, LiDAR, IMU, GNSS, depth, and loop closures. Optimizing these jointly requires a unified sensor-fusion framework rather than per-sensor sequential pipelines. The scene is modeled as an Observation Graph, so optimization should operate directly on that data structure rather than on a private copy.

## Decision

GTSAM is the unified sensor factor-graph engine. GTSAM operates ON the Observation Graph: the graph is the data, and the factors are the algorithm. One optimization jointly adjusts cameras, LiDAR poses, IMU preintegrations, GNSS anchors, and loop-closure edges using the uncertainty/covariance model (covariance ordering translation-then-rotation, per ADR-007). Ceres Solver is retained for smaller nonlinear optimization tasks that do not need GTSAM's factor-graph machinery. GTSAM is registered in THIRD_PARTY.yml and, when invoked as a backend, runs under the worker isolation model (ADR-011); adapters convert between GTSAM conventions and the platform's strict domain types at the boundary.

## Alternatives

- Per-sensor sequential optimization: rejected. Drift and inconsistent maps; no global uncertainty propagation.
- Ceres for everything: rejected. Factor-graph bookkeeping and IMU/GNSS factor support are more natural in GTSAM.
- Custom factor-graph library: rejected. Large maintenance surface and no ecosystem support.

## Consequences

- Positive: one optimization framework across all sensor types; the graph mirrors the Observation Graph; loop closures and GNSS constraints compose naturally; covariance propagates through a single model.
- Negative: GTSAM's ABI and version constraints; translation between domain types (SE(3) = (R, t), quaternion scalar-last) and GTSAM conventions; added dependency weight.
- Risks and mitigations: adapters convert strictly at the boundary; conan.lock pins GTSAM (ADR-003); property-based transform round-trip tests compare against ground-truth transforms; regression tests cover IMU/GNSS/loop-closure fusion cases.

## References

- docs/specifications/scene-model.md
- docs/specifications/geometry-model.md
- docs/specifications/sensor-model.md
- ADR-004 (COLMAP as canonical SfM backend)
- ADR-007 (Coordinate-frame conventions)
