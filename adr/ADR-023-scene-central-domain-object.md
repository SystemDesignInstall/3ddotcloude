# ADR-023 — Scene Is the Central Domain Object

- **Status:** ratified
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

The platform needs a stable mental model and an API for everything the user reconstructs: cameras, sensors, frames, observations, coordinate frames, geometry, and semantics. Early designs risked making "the project" the domain root, conflating two different concerns — the storage container (a `.spx` file) and the reconstruction content (the 3D scene). That conflation breaks when a user wants multiple reconstructions of the same data, or a shared scene across projects. The first architecture principle states that Scene is the primary domain object, so the codebase must be built around it.

## Decision

`Project` is a storage container only: a `.spx` directory holding `project.json`, `project.db`, `artifacts/`, `cache/`, `logs/`, and `temp/`. `Scene` is the domain root and the central domain object, owned by the scene graph (`core/scene/**`). A Scene owns `Sensors`, `Frames`, `Observations`, `CoordinateFrames`, `Geometry`, `SemanticObjects`, and `Appearance`. The Scene is composed of three canonical graphs: the Observation Graph, the Geometry Graph, and the Relationship Graph. All algorithms, the SDK, and the CLI operate on a Scene, never on a Project directly; the Project is only how a Scene is persisted and loaded. Multiple scenes per project are explicitly permitted in the future (e.g. alternative reconstructions of the same captures), so the storage layout and the `scene_id` in provenance and logs are designed from day one to support that. Scene-level provenance records every mutation: which version of which component changed what, with which git commit, all chained into the artifact provenance graph (ADR-015). Scene lifecycle — open, activate, save (atomic), close — is defined in Core; atomic save uses temp-then-rename within the `.spx` directory so a crash never corrupts a saved scene.

## Alternatives

- **Project as the domain root:** rejected — couples storage to content, blocks multiple scenes per project, and contradicts the primary-domain-object principle.
- **Scene as a bag of loose objects:** rejected — loses the graph structure that algorithms and queries rely on; the three-graph model is the substrate everything else uses.
- **Implicit scenes (one per project, unnamed):** rejected — would make the future multi-scene migration a breaking change.

## Consequences

- Positive: the domain model matches the user's mental model (a project is a file; a scene is the content); algorithms and the SDK share one root object; scene-level provenance is auditable; the three-graph structure gives queries and algorithms a canonical substrate; multi-scene support is additive rather than a rewrite.
- Negative: scenes and projects must be mapped to storage carefully; every API must route through Scene, adding ceremony; the scene graph is a larger surface to keep consistent and constitution-protected.
- Risks and mitigations: risk of scene/project conflation creeping into new code — mitigated by `core/scene/**` being constitution-protected and enforced in review; risk of save corruption — mitigated by atomic temp+rename and by persistence tests in CI; risk of provenance growth — mitigated by keeping provenance in manifests, not inline objects.

## References

- `docs/specifications/scene-model.md`
- `docs/architecture/storage-model.md`
- ADR-015 (logging and provenance — scene-level provenance)
- ADR-024 (observation graph — the canonical measurement substrate within the Scene)
- ADR-020 (scheduler persistence — scene state in project.db)
