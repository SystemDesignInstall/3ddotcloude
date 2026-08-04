# ADR-006 — AI outputs are priors

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

Modern AI modules (MASt3R/DUSt3R, VGGT, gsplat, Nerfstudio) produce pose estimates, depth maps, focal lengths, dense correspondences, semantic masks, and dynamic masks. If these outputs were treated as authoritative geometry, silent errors and hallucinations would enter the map. The classical geometric core already provides rigorous validation machinery. The platform must define the role of AI outputs precisely.

## Decision

AI outputs are PRIORS ONLY. Every AI hypothesis — pose prior, depth prior, focal length prior, dense correspondence, semantic mask, dynamic mask, confidence — flows through a classical validation pipeline before it may influence geometry: epipolar consistency, cheirality, reprojection residual, outlier rejection, bundle adjustment, and LiDAR/IMU/GNSS/GCP integration with uncertainty/covariance propagation. AI never writes authoritative geometry; the accept/reject decision is made by the classical core, never by the model. AI outputs that pass validation are recorded with their confidence and provenance in the Relationship Graph. VGGT is used only under a verified commercial license; MASt3R and DUSt3R are used only in research modules due to license uncertainty.

## Alternatives

- Trust AI outputs directly: rejected. Silent geometry corruption, no uncertainty, no auditability.
- Ban AI entirely: rejected. Loses valuable priors for dense matching and pose estimation.
- AI as a capability with no validation gate: rejected. Equivalent to trusting blindly.

## Consequences

- Positive: geometry integrity is guaranteed by classical validation; AI priors still speed up and seed pipelines; research can proceed without license risk in production.
- Negative: AI priors may be rejected, so AI-derived speedups are not guaranteed; additional validation cost; models must be re-validated per release.
- Risks and mitigations: record acceptance/rejection reasons and confidences in provenance; enforce the license gate for VGGT and the research-only carve-out for MASt3R/DUSt3R in CI; validate priors with the same epipolar and residual gates used for classical measurements.

## References

- docs/specifications/geometry-model.md
- docs/specifications/scene-model.md
- ADR-004 (COLMAP as canonical SfM backend)
- ADR-005 (GTSAM as unified sensor factor graph)
- ADR-010 (Content-addressed artifact store)
