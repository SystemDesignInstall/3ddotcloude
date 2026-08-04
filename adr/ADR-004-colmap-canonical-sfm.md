# ADR-004 — COLMAP as canonical SfM backend

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Sparse reconstruction and bundle adjustment are required core capabilities. The capability system lets engines select implementations by declared capability rather than by name, so the default SfM provider must be concrete, mature, and replaceable without changing Core. COLMAP is the de facto open-source SfM/MVS pipeline and covers both SparseReconstruction and BundleAdjustment.

## Decision

COLMAP is the default SfM provider, exposed only behind the Capability/Adapter interface. Its adapter advertises the SparseReconstruction and BundleAdjustment capabilities, and the engine selects it through capability negotiation, never by hard-coded name. Because adapters declare capabilities rather than names, COLMAP can be replaced by OpenMVG, VGGT (under a verified commercial license, per ADR-006), or another implementation without any Core change, consistent with Architecture Principle 15. COLMAP runs out-of-process under the worker isolation model (ADR-011), its outputs are mapped onto world_from_sensor conventions (ADR-007), and its geometry is validated before it is considered authoritative.

## Alternatives

- OpenMVG as default: rejected. Less mature pipeline and tooling, smaller ecosystem.
- VGGT as default: rejected for M0. License is not verified for commercial use; gated by ADR-006.
- In-process SfM calls: rejected. Violates process worker isolation (ADR-011).

## Consequences

- Positive: mature, well-tested SfM; a clean capability interface; a drop-in replacement path; crash isolation via out-of-process execution.
- Negative: COLMAP is C++ heavy with its own dependency graph; adapter work for I/O and coordinate conversion; COLMAP conventions must be translated to world_from_sensor.
- Risks and mitigations: isolate COLMAP behind the capability interface; pin and lock it via Conan (ADR-003); validate its output through epipolar consistency and bundle-adjustment residuals; property-based transform round-trip tests cover the adapter conversions.

## References

- docs/specifications/scene-model.md
- docs/specifications/sensor-model.md
- docs/specifications/geometry-model.md
- ADR-007 (Coordinate-frame conventions)
- ADR-011 (Process worker isolation)
- ADR-006 (AI outputs are priors)
