# Spatial Platform — Architecture Constitution

Status: **Ratified (P0)**

This document is the supreme governing document of the Spatial Platform architecture. It outranks all other documentation except the product-level `platform-blueprint.md` for strategic direction. It can only be changed through the RFC process defined in `README.md`.

Every proposed change to any part of the platform is evaluated against this document. A change that violates the Constitution is rejected even if it passes all functional tests.

---

## 1. Architecture Principles

These are the evaluation criteria for all changes. Each principle is a hard rule, not a guideline.

1. **Scene is the primary domain object.** All processing, storage, provenance, and products revolve around the `Scene`. `Project` is a storage container, never the domain root.
2. **Geometry is algorithm-independent.** Geometry is stored as `GeometryElement`s. No geometry producer (SfM, SLAM, LiDAR, Gaussian Splatting, NeRF, TSDF, CAD/BIM, manual, generative) may force its native representation into the Core.
3. **Observations are immutable measurements.** An `Observation` is a fact recorded by a sensor at a time and place. It is never mutated; corrections produce new observations.
4. **AI never produces authoritative geometry.** AI models emit only hypotheses (pose/depth/focal priors, dense correspondences, semantic masks, dynamic masks, confidence). The classical geometric core validates, rejects outliers, performs bundle adjustment, and computes uncertainty before any hypothesis becomes geometry.
5. **Every transformation has explicit coordinate frames.** A transform without named source and target frames is invalid. Raw matrices and untyped scalars are forbidden in business logic (see ADR-007, ADR-018).
6. **All processing is reproducible.** Any pipeline run can be re-executed to produce bit-identical outputs given identical inputs, recipe, code version, and configuration.
7. **Artifacts are immutable.** A computed result is written once, addressed by content, and never modified. New results are new artifacts.
8. **Capabilities are preferred over implementation names.** Components declare what they can do (`Capability`), not which tool they are. The engine selects implementations by capability.
9. **Plugins never bypass Core.** Third-party code reaches the system exclusively through the Plugin → Adapter → Capability interfaces. No plugin writes to Core structures directly.
10. **Public contracts evolve only through RFC.** Any change to the Constitution-protected surfaces requires a numbered RFC and Architecture Review.
11. **Domain types are preferred over primitive types.** `TimestampNs`, `DistanceMeters`, `AngleRadians`, `WorldFromCamera`, `CameraFromWorld`, `RigFromSensor`, `SensorFromRig` replace `double`, `int64_t`, and `Matrix4d` in all public and business-layer interfaces.
12. **Provenance is never discarded.** Every artifact and every geometry element retains the chain of inputs, producers, and configuration that created it.
13. **Every geometry element has an explicit coordinate frame.** An element whose frame is unknown is a defect.
14. **Quality metadata is first-class.** Uncertainty, covariance, confidence, and residuals are data, carried with the geometry, not derived on demand.
15. **Every algorithm is replaceable.** Replacing COLMAP with OpenMVG, VGGT, or a future engine must not require changes to Core, Scene, or Artifact Store.
16. **Every algorithm is ephemeral; every observation is permanent.** Images, observations, and Scenes persist as immutable, versioned, provenance-rich data for the lifetime of the platform. Reconstruction engines (COLMAP, VGGT, KISS-ICP, MASt3R, FAST-LIO2, future models) are temporary interpretations of those observations and are always replaceable without altering the data model.

---

## 2. Constitution-Protected Surfaces

The following surfaces are frozen. No agent, team, or engineer may change them without a ratified RFC and passing Architecture Review.

| Surface | Meaning | Protected paths (repo) |
|---|---|---|
| Coordinate System | Convention set, units, transforms, strict types | `core/coordinates/**`, `core/geometry/**` |
| Scene | The `Scene` domain model and its graphs | `core/scene/**` |
| Observation | The `Observation` hierarchy and Observation Graph | `core/scene/observation_graph/**` |
| GeometryElement | The geometry abstraction and its concrete types | `core/scene/geometry/**` |
| Artifact Format | CAS layout, manifest, metadata, atomicity | `core/artifacts/**` |
| UUID | Canonical identifier format and assignment rules | `core/**` (type `Uuid`), `schemas/**` |
| Plugin API | Plugin, PluginManager, Capability, Adapter interfaces | `core/plugin/**`, `adapters/interfaces/**` |
| Capability API | The capability taxonomy and negotiation | `core/plugin/**`, `schemas/**` |

Additionally, schema contracts under `schemas/**` (protobuf, JSON Schema, SQL DDL) are protected because they are cross-language interfaces.

**Change-control rule:** any modification of a protected path must reference a ratified RFC in the commit/PR body (e.g. `RFC-0042`). CI enforces this (`constitution-check`).

---

## 3. What May Change Without an RFC

- Internal implementation of any adapter behind its interface.
- Purely additive internal refactoring that preserves public behavior.
- Tooling, CI, tests, and documentation that do not touch protected surfaces.
- Choice and version of algorithm backends (COLMAP → OpenMVG → VGGT), as long as the Capability/Adapter contract is respected.

---

## 4. Non-Negotiable Constraints

- No large binary data in SQLite. SQLite stores metadata and indices only; payloads live in the content-addressed artifact store (ADR-008, ADR-009).
- No implicit units or implicit coordinate frames anywhere in public APIs (ADR-007).
- No absolute local filesystem paths in persisted state; portable URI abstraction only (ADR-008).
- No TODO/FIXME/HACK or temporary structures in `core/**`, `engine/**`, `schemas/**`. Architecture Debt = 0 in the kernel (allowed in `python/research/**`, `benchmarks/experimental/**`).
- No "quick prototype then rewrite" in the kernel. A task is either built to the architecture or deferred (ADR-031).

---

## 5. Architecture Review Board

Any PR touching protected paths requires Architecture Review, separate from Code Review:

1. Reviewer checks compliance with this Constitution and the 16 principles.
2. The change must cite its RFC (or be explicitly non-contractual and additive).
3. Domain-type and debt checks (`check-domain-types`, `check-arch-debt`) must pass.
4. Unresolved objections block the PR until a follow-up RFC is opened.

---

*Amendments to this document require a ratified RFC that explicitly modifies this section and a two-thirds Architecture Board approval.*
