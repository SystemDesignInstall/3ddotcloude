# ADR-039 — PPS-0001 Introduction

- **Status:** ratified
- **Owner:** Architecture Board
- **Date:** 2026-08-07
- **Supersedes:** none

## Context

The platform has ratified its two foundational RFCs — RFC-0002 (Permanent Spatial Data Model) and RFC-0003 (Processing Engine and Execution Architecture) — and completed its P1/P1.5 infrastructure (core, artifact store, scheduler, workers, CLI, quality engine). The next major block is P2 (Photogrammetry Core). Before P2 begins, the platform needs an architectural invariant layer that binds RFC-0002 and RFC-0003 together and prevents engine coupling and data-model drift as algorithms (COLMAP, OpenMVG, AliceVision, neural reconstructions, Gaussian Splatting) are introduced and replaced over the life of the platform. Without such a layer, external library types can leak into the core and ad hoc artifact shapes can accumulate until the data model can no longer be relied upon.

## Decision

Introduce **PPS-0001 — Platform Principles Specification** (`docs/PPS-0001-platform-principles.md`) as the architectural invariant layer governing Spatial Platform evolution.

PPS-0001 fixes the normative invariants of the domain layer: the Canonical Spatial Data Model, Artifact-Centric Architecture, Artifact Immutability, the Stable Identity System, Coordinate Frame Rules, Adapter Isolation, the Provenance Requirement, Canonical Naming Rules, and Algorithm Independence. It also fixes recommended policies (numeric representation, uncertainty propagation, deterministic execution), extension guidelines (capability evolution, future artifact types, long-term vision), and a Compliance Checklist against which every architectural change is evaluated.

Its hierarchy in the architecture:

```
CONSTITUTION.md
        │
        ▼
PPS-0001 — Platform Principles Specification
        │
   ┌────┴────┐
   ▼         ▼
RFC-0002    RFC-0003
```

- CONSTITUTION — philosophy and change control;
- PPS-0001 — architectural invariants of the domain layer;
- RFC-0002 — the spatial data model;
- RFC-0003 — the processing engine.

The core rule of PPS-0001: **no external library type crosses the Adapter Boundary**, and the Scheduler manages Processing Capabilities, never libraries.

## Alternatives

- **Encode the invariants inside a new RFC instead of a PPS.** Rejected: the invariants are not a proposal subject to RFC lifecycle; they are a formalization of constraints already ratified in RFC-0002/RFC-0003 and the CONSTITUTION. A separate specification level avoids treating a stable baseline as changeable policy.
- **Make the photogrammetry pipeline specification (a domain contract) the invariant document.** Rejected: that would mix architecture rules with photogrammetry implementation details, and would not govern SLAM, LiDAR, AI, and future domains.
- **No new document; rely on the CONSTITUTION and ADRs.** Rejected: the CONSTITUTION fixes philosophy, and ADRs are point decisions; neither provides a single compliance checklist for the domain layer, which PPS-0001 now provides.
- **ADR number 038.** Rejected: ADR-038 is already ratified (Processing Engine boundary definition); the next free number is 039.

## Consequences

- Positive:
  - Algorithms become replaceable — replacing COLMAP with OpenMVG or a neural reconstruction does not require changes to Core, Scene, or the Artifact Store.
  - RFC-0002 and RFC-0003 gain a single, shared foundation with an explicit compliance checklist.
  - Future technologies (Gaussian Splatting, neural reconstruction, AI scene understanding) can be integrated without breaking the data model.
  - The compliance checklist turns the architecture rules into checkable criteria for agents and reviewers.
- Negative:
  - Requires discipline in the Adapter Layer; adapters must convert to the canonical model and may not leak external types.
  - Increases the amount of architectural documentation that must be maintained alongside code.
- Risks and mitigations:
  - Risk: PPS-0001 is interpreted as exhaustive and stifles evolution. Mitigation: the document explicitly marks extension guidelines and future capability/artifact namespaces as non-normative and open.
  - Risk: drift between PPS-0001 and later pipeline specifications. Mitigation: the Compliance Checklist in §8 makes the rules checkable; pipeline specifications must not contradict PPS-0001.

## References

- `docs/PPS-0001-platform-principles.md`
- CONSTITUTION.md (§1, §2)
- RFC-0002, RFC-0003
- ADR-004, ADR-007, ADR-010, ADR-013, ADR-018, ADR-020, ADR-025, ADR-030, ADR-033, ADR-034, ADR-038
