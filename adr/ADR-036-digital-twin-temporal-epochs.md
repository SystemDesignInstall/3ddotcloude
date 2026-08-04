# ADR-036 — Digital Twin and Temporal Epochs

- **Status:** accepted
- **Owner:** Spatial Platform Architecture Board
- **Date:** 2026-08-04
- **Supersedes:** none

## Context

The roadmap ends in Digital Twin: scenes that persist over time, change, merge, and are monitored — not one-shot reconstructions. Real deployments will re-capture a site repeatedly, detect change between captures, merge captures, and raise alerts on geometric deviation. None of that processing will ship soon (it is years downstream per the blueprint), but the data model we ratify now determines whether temporal features are ever possible. If the Scene cannot express "this capture is an epoch of a continuing twin", change detection and monitoring are structurally impossible to add later. The Constitution-protected Scene surface is effectively frozen; retrofitting time into it after ratification would be catastrophic.

## Decision

The Scene data model accommodates temporal semantics from the start, even though temporal processing is deferred: a Scene carries a temporal dimension placeholder, and every observation and geometry element is timestamped in the canonical `TimestampNs` domain type. Versioning (ADR-033) already chains states over processing time; epochs extend this with capture time: a Digital Twin Scene organizes epochs (time-bounded captures) that share a stable twin identity, support change detection, merge, monitoring, updates, and versioning. Change detection and merge are defined as epoch-level operations over scene versions, and monitoring consumes the quality engine's expected-error signals (ADR-030). The schema ratified in P0 includes epoch identity and capture timestamps; the engine processing is deferred years, but no later ADR will need to modify the protected Scene surface to express time.

**Deferred to:** Digital Twin milestone — epoch processing, change detection, merge, and monitoring; the data model is prepared in P0.

## Alternatives

- **Postpone all temporal design:** rejected — the protected Scene surface would then need a breaking temporal retrofit after ratification.
- **Full temporal processing now:** rejected — outside M0 and later milestones; no users, no evidence base, and contradicts ADR-031.
- **Time as free-form metadata:** rejected — untyped time cannot drive change detection, merge, or monitoring in a principled way.

## Consequences

- Positive: temporal features remain structurally possible no matter when they ship; capture timestamps exist on all observations from M0 onward, so provenance gains a time dimension; no future protected-surface break is required for time.
- Negative: a small schema surface is reserved for a deferred feature; epoch identity must be maintained even when unused; there is temptation to pre-build processing that belongs to a later milestone.
- Risks and mitigations: risk of over-engineering the placeholder — mitigated by keeping the P0 surface minimal (twin identity, epoch identity, capture timestamps); risk of clock/synchronization inconsistencies across sensors — mitigated by `TimestampNs` strict typing and observation provenance; risk of scope creep — mitigated by ADR-031's explicit deferral.

## References

- `docs/specifications/scene-model.md`
- `docs/specifications/geometry-model.md`
- ADR-023 (scene as central object), ADR-024 (observation graph), ADR-030 (quality engine), ADR-031 (M0 scope), ADR-033 (scene versioning)
